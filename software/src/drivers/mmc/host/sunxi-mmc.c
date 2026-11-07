/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file sunxi-mmc.c
 * @brief SMHC0, the SD/MMC host controller.
 *
 * Clocks, bus width, the polled command engine with its internal DMA, the data sample point and
 * the I/O supply switch. The SD protocol on top of it lives in core/sd.c; this file knows
 * commands only as opcodes and response shapes.
 */

/** @brief Log module name used by the pr_*() helpers. */
#define LOG_MODULE "mmc"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <hal/cache.h>
#include <hal/io.h>
#include <hal/kernel.h>
#include <init.h>
#include <log.h>

#include "arch/arch.h"
#include "drivers/drivers.h"
#include "drivers/mmc/host/sunxi-mmc.h"
#include "drivers/mmc/mmc-internal.h"

/* ---- registers ---- */

/** @brief Global control and reset bits. */
#define SMHC_GCTRL  0x00U
/** @brief Card clock control: divider and clock enable. */
#define SMHC_CLKCR  0x04U
/** @brief Data and response timeouts. */
#define SMHC_TMOUT  0x08U
/** @brief Data bus width. */
#define SMHC_WIDTH  0x0cU
/** @brief Data block size. */
#define SMHC_BLKSZ  0x10U
/** @brief Data block count. */
#define SMHC_BCNTR  0x14U
/** @brief Command and its flags. */
#define SMHC_CMDR   0x18U
/** @brief Command argument. */
#define SMHC_CARG   0x1cU
/** @brief First of the four response words. */
#define SMHC_RESP0  0x20U
/** @brief Raw interrupt mask. */
#define SMHC_IMASK  0x30U
/** @brief Raw interrupt status. */
#define SMHC_RINTR  0x38U
/** @brief Controller and card status. */
#define SMHC_STAS   0x3cU
/** @brief FIFO threshold control. */
#define SMHC_FTRGL  0x40U
/** @brief CRC status detection control. */
#define SMHC_CSDC   0x54U
/** @brief Auto-stop argument for a multi-block transfer. */
#define SMHC_A12A   0x58U
/** @brief New timing mode: drive phases and sampling delay. */
#define SMHC_NTSR   0x5cU
/** @brief Internal DMA control. */
#define SMHC_DMAC   0x80U
/** @brief Descriptor list base address. */
#define SMHC_DLBA   0x84U
/** @brief Internal DMA status. */
#define SMHC_IDST   0x88U
/** @brief Internal DMA interrupt enable. */
#define SMHC_IDIE   0x8cU
/** @brief Read and write FIFO thresholds. */
#define SMHC_THLD   0x100U
/** @brief Command and data drive delays. */
#define SMHC_DRV_DL 0x140U
/** @brief Data sampling delay. */
#define SMHC_SAMP_DL 0x144U
/** @brief The data FIFO. */
#define SMHC_FIFO   0x200U

/** @brief GCTRL: reset the controller state machine. */
#define GCTRL_SOFT_RST	      (1U << 0)
/** @brief GCTRL: reset the FIFO pointers. */
#define GCTRL_FIFO_RST	      (1U << 1)
/** @brief GCTRL: reset the internal DMA engine. */
#define GCTRL_DMA_RST	      (1U << 2)
/** @brief GCTRL: all three resets at once. */
#define GCTRL_ALL_RST	      (GCTRL_SOFT_RST | GCTRL_FIFO_RST | GCTRL_DMA_RST)
/** @brief GCTRL: route the data path through the internal DMA. */
#define GCTRL_INT_EN	      (1U << 4)
/** @brief GCTRL: enable the internal DMA engine. */
#define GCTRL_DMA_EN	      (1U << 5)
/** @brief GCTRL: the access-done direction bit. */
#define GCTRL_ACCESS_DONE_DIR (1U << 30)
/** @brief GCTRL: move data through the AHB FIFO path instead of the DMA engine. */
#define GCTRL_ACCESS_BY_AHB   (1U << 31)

/** @brief CLKCR: clock divider field. */
#define CLKCR_DIV_MASK	 0xffU
/** @brief CLKCR: card clock enable. */
#define CLKCR_CARD_CLK_ON (1U << 16)
/** @brief CLKCR: mask DAT0 during a clock update. */
#define CLKCR_MASK_DATA0  (1U << 31)

/** @brief CMDR: command index field. */
#define CMDR_OPCODE_MASK 0x3fU
/** @brief CMDR: a response is expected. */
#define CMDR_RSP_EXP	 (1U << 6)
/** @brief CMDR: the response is 136 bits, not 48. */
#define CMDR_LONG_RSP	 (1U << 7)
/** @brief CMDR: check the response CRC. */
#define CMDR_CHECK_CRC	 (1U << 8)
/** @brief CMDR: the command has a data phase. */
#define CMDR_DATA_EXP	 (1U << 9)
/** @brief CMDR: the data phase goes to the card. */
#define CMDR_WRITE	 (1U << 10)
/** @brief CMDR: send the stop command after a multi-block transfer. */
#define CMDR_AUTO_STOP	 (1U << 12)
/** @brief CMDR: wait for the previous command to finish first. */
#define CMDR_WAIT_PRE_OVER (1U << 13)
/** @brief CMDR: send the 80-clock initialisation sequence before the command. */
#define CMDR_SEND_INIT_SEQ (1U << 15)
/** @brief CMDR: run the command only to update the clock. */
#define CMDR_UPCLK_ONLY	 (1U << 21)
/** @brief CMDR: the controller performs the voltage switch handshake. */
#define CMDR_VOLT_SWITCH (1U << 28)
/** @brief CMDR: start the command. */
#define CMDR_START	 (1U << 31)

/** @brief RINTR: command response error. */
#define INT_RESP_ERR	  (1U << 1)
/** @brief RINTR: command done. */
#define INT_CMD_DONE	  (1U << 2)
/** @brief RINTR: data phase done. */
#define INT_DATA_OVER	  (1U << 3)
/** @brief RINTR: response CRC mismatch. */
#define INT_RESP_CRC_ERR  (1U << 6)
/** @brief RINTR: data CRC mismatch. */
#define INT_DATA_CRC_ERR  (1U << 7)
/** @brief RINTR: no response within the timeout. */
#define INT_RESP_TIMEOUT  (1U << 8)
/** @brief RINTR: data phase timed out. */
#define INT_DATA_TIMEOUT  (1U << 9)
/** @brief RINTR: the voltage switch handshake completed. */
#define INT_VOLT_DONE	  (1U << 10)
/** @brief RINTR: the FIFO underran or overran. */
#define INT_FIFO_RUN_ERR  (1U << 11)
/** @brief RINTR: the card interface locked up. */
#define INT_HARD_LOCKED	  (1U << 12)
/** @brief RINTR: bad start bit. */
#define INT_START_BIT_ERR (1U << 13)
/** @brief RINTR: the automatic stop command completed. */
#define INT_AUTO_CMD_DONE (1U << 14)
/** @brief RINTR: bad end bit. */
#define INT_END_BIT_ERR	  (1U << 15)
/** @brief Every interrupt bit that means the transfer failed. */
#define INT_ERRORS                                                                                 \
	(INT_RESP_ERR | INT_RESP_CRC_ERR | INT_DATA_CRC_ERR | INT_RESP_TIMEOUT | INT_DATA_TIMEOUT | \
	 INT_FIFO_RUN_ERR | INT_HARD_LOCKED | INT_START_BIT_ERR | INT_END_BIT_ERR)

/** @brief STAS: the FIFO is empty. */
#define STAS_FIFO_EMPTY (1U << 2)
/** @brief STAS: the FIFO is full. */
#define STAS_FIFO_FULL	(1U << 3)
/** @brief STAS: the card holds DAT0 low. */
#define STAS_CARD_BUSY	(1U << 9)

/* new timing mode: sample phases, drive phases and the sampling delay */
/** @brief NTSR: enable the new timing mode (double-rate clock). */
#define NTSR_2X_TIMING	    (1U << 31)
/** @brief NTSR: data sampling phase field. */
#define NTSR_DAT_PH_MASK    (3U << 8)
/** @brief DRV_DL: drive the command half a cycle ahead. */
#define DRV_DL_CMD_PH	    (1U << 16)
/** @brief DRV_DL: drive the data half a cycle ahead. */
#define DRV_DL_DAT_PH	    (1U << 17)
/** @brief SAMP_DL: enable the explicit sampling delay. */
#define SAMP_DL_ENABLE	    (1U << 7)
/** @brief SAMP_DL: sampling delay field. */
#define SAMP_DL_MASK	    0x3fU
/** @brief CSDC: enable CRC status detection on writes. */
#define CSDC_CRC_STATUS_DETECT 3U
/** @brief CSDC: CRC status detection field. */
#define CSDC_CRC_MASK	    0xfU

/** @brief The FIFO holds 1 KiB; a fast read waits for a whole block before the CPU drains it. */
#define FIFO_BYTES	    1024U
/** @brief Bytes left in the FIFO before a read threshold is crossed. */
#define RX_WATERMARK_BYTES  28U
/** @brief Bytes in the FIFO before a write threshold is crossed. */
#define TX_WATERMARK_BYTES  992U
/** @brief THLD: enable the read threshold. */
#define THLD_RD_EN	    (1U << 0)
/** @brief THLD: enable the write threshold. */
#define THLD_WR_EN	    (1U << 2)
/** @brief THLD: threshold size field. */
#define THLD_SIZE_MASK	    (0xfffU << 16)

/* the internal DMA controller and its descriptor list */
/** @brief DMAC: soft-reset the DMA engine. */
#define DMAC_SOFT_RST  (1U << 0)
/** @brief DMAC: use fixed-length bursts. */
#define DMAC_FIX_BURST (1U << 1)
/** @brief DMAC: enable the engine. */
#define DMAC_ON	       (1U << 7)
/** @brief Descriptor: disable the interrupt after this descriptor. */
#define DESC_DIC       (1U << 1)
/** @brief Descriptor: last descriptor of the list. */
#define DESC_LD	       (1U << 2)
/** @brief Descriptor: first descriptor of the list. */
#define DESC_FD	       (1U << 3)
/** @brief Descriptor: chain to the next descriptor. */
#define DESC_CH	       (1U << 4)
/** @brief Descriptor: raise an interrupt when this descriptor ends. */
#define DESC_ER	       (1U << 5)
/** @brief Descriptor: ownership bit the engine clears when done. */
#define DESC_OWN       (1U << 31)
/** @brief Largest buffer one descriptor may describe. */
#define DESC_MAX_BYTES 8192U
/** @brief Number of descriptors: 128 KiB, the largest block command. */
#define DESC_COUNT     16U
/** @brief Descriptor addresses are stored divided by four. */
#define DESC_ADDRESS_SHIFT 2U
/** @brief IDST: the receive transfer finished. */
#define IDST_RX_INT    (1U << 1)
/** @brief IDST: every DMA error bit. */
#define IDST_ERRORS    ((1U << 2) | (1U << 4) | (1U << 5) | (1U << 9))
/** @brief FTRGL: DMA burst size in the field's units. */
#define DMA_BURST_SIZE 2U
/** @brief FTRGL: receive watermark in 32-bit words. */
#define DMA_RX_WATERMARK 7U
/** @brief FTRGL: transmit watermark in 32-bit words. */
#define DMA_TX_WATERMARK 248U
/** @brief Largest transfer the bounce buffer and descriptor list can take. */
#define DMA_BOUNCE_BYTES (DESC_COUNT * DESC_MAX_BYTES)

/* the card port's supply switch inside the chip: 1 is 3.3 V, 0 is 1.8 V, bit 5 reports it */
/** @brief Pad voltage control register of the card port; 1 selects 3.3 V. */
#define PAD_VOLTAGE_CONTROL (SUNXI_GPIO_BASE + 0x390U)
/** @brief Pad voltage status register; bit 5 reports the level in use. */
#define PAD_VOLTAGE_STATUS  (SUNXI_GPIO_BASE + 0x38cU)
/** @brief Status bit that reports the pad voltage. */
#define PAD_VOLTAGE_MASK    (1U << 5)

/* ---- the controller's state ---- */

/** @brief Board data of the controller in use. */
static const struct sunxi_mmc_config *host_config;
/** @brief Card clock last programmed, in Hz; 0 before the first one. */
static uint32_t host_clock;
/** @brief Data bus width last programmed, in lines. */
static uint32_t host_width = 1U;
/** @brief SDR50/SDR104: the read FIFO needs a whole-block threshold. */
static bool host_fast;
/** @brief A tuning scan expects the failed reads, so they are not logged. */
static bool host_quiet;
/** @brief Transfers that used the caller's buffer directly. */
static uint32_t dma_direct_count;
/** @brief Transfers that went through the bounce buffer. */
static uint32_t dma_bounce_count;

/**
 * @brief One entry of the internal DMA descriptor list.
 *
 * @details The engine walks the list in memory; addresses are stored shifted right by two and
 * the ownership bit is cleared by the engine as it finishes each entry.
 */
struct smhc_descriptor {
	/** @brief Flags: first, last, chained, ownership and interrupt control. */
	uint32_t config;
	/** @brief Bytes described by this entry. */
	uint32_t size;
	/** @brief Buffer address, shifted right by @c DESC_ADDRESS_SHIFT. */
	uint32_t buffer_address;
	/** @brief Next descriptor address, shifted right by @c DESC_ADDRESS_SHIFT. */
	uint32_t next_address;
};

/** @brief The descriptor list; cache line aligned so a clean covers whole lines. */
static struct smhc_descriptor dma_descriptors[DESC_COUNT] __attribute__((aligned(CACHE_LINE_BYTES)));
/** @brief Bounce buffer for transfers that are not cache line aligned or sized. */
static uint8_t dma_bounce[DMA_BOUNCE_BYTES] __attribute__((aligned(CACHE_LINE_BYTES)));

/* ---- register helpers ---- */

/**
 * @brief Read one controller register.
 *
 * @param[in] offset Register offset from the controller base.
 * @return The register value.
 */
static uint32_t smhc_read(uint32_t offset)
{
	return readl(host_config->base + offset);
}

/**
 * @brief Write one controller register.
 *
 * @param[in] value  Value to write.
 * @param[in] offset Register offset from the controller base.
 */
static void smhc_write(uint32_t value, uint32_t offset)
{
	writel(value, host_config->base + offset);
}

/**
 * @brief Busy-wait for a short while.
 *
 * @param[in] microseconds Delay in microseconds.
 */
static void smhc_delay_us(uint32_t microseconds)
{
	cs_udelay(microseconds);
}

/**
 * @brief Wait until none of the given register bits is set.
 *
 * @details Used after every reset and start request, where the bit clears when the controller
 * is done.
 *
 * @param[in] offset     Register offset.
 * @param[in] bits       Bits that must clear.
 * @param[in] timeout_ms Time limit in milliseconds.
 * @return 0 when the bits cleared, or -ETIMEDOUT.
 */
static int smhc_wait_clear(uint32_t offset, uint32_t bits, uint32_t timeout_ms)
{
	uint64_t deadline = ktime_get_ms() + timeout_ms;

	while ((smhc_read(offset) & bits) != 0U) {
		if (ktime_get_ms() > deadline) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/**
 * @brief Choose the module clock divider for a target rate.
 *
 * @details Module clock: HOSC 24 MHz (mux 0) below 12 MHz, the 400 MHz peripheral PLL above.
 * The divider is M * 2^P with M in 1..16 and P in 0..3; the function evaluates all four powers
 * and keeps the combination with the smallest absolute error, returning the register value
 * with P in the high byte and M-1 in the low byte.
 *
 * @param[in] source Source clock in Hz.
 * @param[in] target Wanted output in Hz.
 * @return The divider register value.
 */
static uint32_t module_clock_divider(uint32_t source, uint32_t target)
{
	uint32_t best_value = 0U;
	uint32_t best_error = 0xffffffffU;

	/* the divider is M * 2^P with M in 1..16 and P in 0..3; pick the closest output */
	for (uint32_t power = 0U; power < 4U; power++) {
		uint32_t divided = target << power;
		uint32_t multiplier = (source + divided / 2U) / divided;
		uint32_t got;
		uint32_t error;

		if (multiplier < 1U) {
			multiplier = 1U;
		}
		if (multiplier > 16U) {
			multiplier = 16U;
		}
		got = source / (multiplier << power);
		error = got > target ? got - target : target - got;
		if (error < best_error) {
			best_error = error;
			best_value = (power << 8) | (multiplier - 1U);
		}
	}
	return best_value;
}

/**
 * @brief Program the module clock of the controller.
 *
 * @details The controller takes a module clock of twice the card clock, so the internal divider
 * stays 0. Only the 400 MHz output of the peripheral PLL is used above 12 MHz; the 300 MHz one
 * does not clock this controller's card interface reliably on this board. The CCU register is
 * written with its enable bit set.
 *
 * @param[in] card_clock Wanted card clock in Hz.
 */
static void smhc_set_module_clock(uint32_t card_clock)
{
	uint32_t target = card_clock * 2U;
	uint32_t value;

	/* only the 400 MHz output of the peripheral PLL is used: the 300 MHz one does not
	 * clock this controller's card interface reliably on this board */
	if (target > 12000000U) {
		value = (1U << 24) | module_clock_divider(400000000U, target);
	} else {
		value = module_clock_divider(24000000U, target);
	}
	writel(value | (1U << 31), SUNXI_CCU_BASE + host_config->clock_register);
}

/**
 * @brief Stop or start the card clock.
 *
 * @details The controller latches the change on its own command. During the 1.8 V sequence the
 * command carries the voltage switch flag so the controller takes part. DAT0 is masked while
 * the update command runs, because the card may be holding it; every pending interrupt is
 * cleared afterwards so a stale bit does not look like a completion. A command still sitting
 * in the engine blocks the update, which is reported as a timeout.
 *
 * @param[in] on      true to start the clock, false to stop it.
 * @param[in] voltage true to take part in the 1.8 V switch handshake.
 * @return 0 on success, or -ETIMEDOUT when the update did not complete.
 */
static int smhc_clock_switch(bool on, bool voltage)
{
	uint32_t clkcr = smhc_read(SMHC_CLKCR) & ~(CLKCR_CARD_CLK_ON | CLKCR_DIV_MASK);

	if (on) {
		clkcr |= CLKCR_CARD_CLK_ON;
	}
	smhc_write(clkcr, SMHC_CLKCR);
	smhc_write(clkcr | CLKCR_MASK_DATA0, SMHC_CLKCR);
	smhc_write(CMDR_START | CMDR_UPCLK_ONLY | CMDR_WAIT_PRE_OVER | (voltage ? CMDR_VOLT_SWITCH : 0U),
		SMHC_CMDR);
	if (smhc_wait_clear(SMHC_CMDR, CMDR_START, 100U) != 0) {
		pr_err("clock update (%s) stuck: cmdr %08x stas %08x gctrl %08x\n",
			on ? "start" : "stop", (uint32_t)smhc_read(SMHC_CMDR),
			(uint32_t)smhc_read(SMHC_STAS), (uint32_t)smhc_read(SMHC_GCTRL));
		return -ETIMEDOUT;
	}
	smhc_write(0xffffffffU, SMHC_RINTR);
	smhc_write(clkcr, SMHC_CLKCR);
	return 0;
}

/**
 * @brief Reset the command and DMA engines and restore the bus width.
 *
 * @details A command still sitting in the engine blocks the clock stop; reset it and start
 * over. Every pending interrupt and DMA status bit is cleared, so the next command starts from
 * a clean slate.
 */
static void smhc_reset_engine(void)
{
	smhc_write(smhc_read(SMHC_GCTRL) | GCTRL_ALL_RST, SMHC_GCTRL);
	(void)smhc_wait_clear(SMHC_GCTRL, GCTRL_ALL_RST, 100U);
	smhc_write(0xffffffffU, SMHC_RINTR);
	smhc_write(0xffffffffU, SMHC_IDST);
	smhc_write(host_width == 4U ? 1U : host_width == 8U ? 2U : 0U, SMHC_WIDTH);
}

/**
 * @brief Set the card clock and the timing of the data interface.
 *
 * @details Delegates the divider to smhc_set_module_clock() and wraps it in a clock stop and
 * start, since the divider may only change while the card clock is off. A failed stop is
 * usually a stale command left by a failed tuning read, so it is reset once and retried. After
 * the clock is running the drive and sample delays are set: the command is driven half a cycle
 * ahead, and for the 26-52 MHz range the data is also driven early and sampled at the
 * controller's own point. Above 52 MHz the interface is marked fast and the sample point is
 * left to the protocol layer's tuning. CRC status detection is enabled and any explicit
 * sampling delay cleared.
 *
 * @param[in] clock Wanted card clock in Hz.
 * @return 0 on success, or the error of the clock update.
 */
int sunxi_mmc_set_clock(uint32_t clock)
{
	uint32_t clkcr = smhc_read(SMHC_CLKCR) & ~(CLKCR_CARD_CLK_ON | CLKCR_DIV_MASK);
	uint32_t drive;
	uint32_t timing;
	uint32_t sample = 0U;
	int result = smhc_clock_switch(false, false);

	if (result != 0) {
		/* often a stale command (a failed tuning read) left in the command engine */
		smhc_reset_engine();
		result = smhc_clock_switch(false, false);
	}
	if (result != 0) {
		return result;
	}
	/* the module clock is twice the card clock, so the divider stays 0 */
	smhc_write(clkcr, SMHC_CLKCR);
	smhc_set_module_clock(clock);
	smhc_write(clkcr, SMHC_CLKCR);
	pr_debug("clock %u Hz: module 0x%08x clkcr 0x%08x\n", (uint32_t)clock,
		(uint32_t)readl(SUNXI_CCU_BASE + host_config->clock_register), (uint32_t)smhc_read(SMHC_CLKCR));
	result = smhc_clock_switch(true, false);
	if (result != 0) {
		return result;
	}

	host_clock = clock;
	host_fast = clock > 52000000U;
	/* command drive half a cycle ahead; the data drive shift and the sample point only for
	 * the high speed mode of the 3.3 V signalling, the UHS modes find their own point */
	drive = DRV_DL_CMD_PH;
	timing = NTSR_2X_TIMING;
	if (clock > 26000000U && clock <= 52000000U) {
		drive |= DRV_DL_DAT_PH;
		sample = 1U;
	}
	smhc_write(drive, SMHC_DRV_DL);
	smhc_write(timing | (sample << 4) | (sample << 8), SMHC_NTSR);
	smhc_write((smhc_read(SMHC_CSDC) & ~CSDC_CRC_MASK) | CSDC_CRC_STATUS_DETECT, SMHC_CSDC);
	smhc_write(0U, SMHC_SAMP_DL);
	return 0;
}

/**
 * @brief Select the bus width of the card interface.
 *
 * @details The register encodes the width as a field, not a count; the chosen width is kept so
 * a reset or recovery can restore it.
 *
 * @param[in] width Number of data lines: 1, 4 or 8.
 */
void sunxi_mmc_set_width(uint32_t width)
{
	host_width = width;
	smhc_write(width == 4U ? 1U : width == 8U ? 2U : 0U, SMHC_WIDTH);
}

/**
 * @brief Set the I/O supply of the card port.
 *
 * @details Writes the pad voltage control and waits up to 100 * 100 us for the status bit to
 * report the new level, then always waits another 10 ms for the supply to settle. The wait
 * does not fail: a board without the status bit still gets the settling time.
 *
 * @param[in] millivolt 3300 or 1800.
 */
void sunxi_mmc_set_voltage(uint32_t millivolt)
{
	uint32_t want = millivolt == 3300U ? 1U : 0U;

	writel(want, PAD_VOLTAGE_CONTROL);
	for (uint32_t waited = 0U; waited < 100U; waited++) {
		if (((readl(PAD_VOLTAGE_STATUS) & PAD_VOLTAGE_MASK) != 0U) == (want != 0U)) {
			break;
		}
		cs_udelay(100U);
	}
	cs_udelay(10000U);
}

/**
 * @brief The host half of the CMD11 sequence.
 *
 * @details Stops the card clock with the controller taking part in the switch, changes the
 * supply, waits 10 ms and starts the clock again with the same flag.
 *
 * @param[in] millivolt 3300 or 1800.
 * @return 0 on success, or the error of either clock update.
 */
int sunxi_mmc_voltage_switch(uint32_t millivolt)
{
	int result = smhc_clock_switch(false, true);

	if (result != 0) {
		return result;
	}
	sunxi_mmc_set_voltage(millivolt);
	msleep(10U);
	return smhc_clock_switch(true, true);
}

/**
 * @brief Report whether the card holds DAT0 low.
 *
 * @return true when the card is busy.
 */
bool sunxi_mmc_card_busy(void)
{
	return (smhc_read(SMHC_STAS) & STAS_CARD_BUSY) != 0U;
}

/**
 * @brief Report whether a card is in the slot.
 *
 * @details A board without a card detect pin (pin 0xff) always reports a card.
 *
 * @return true when the detect pin is active low.
 */
bool sunxi_mmc_present(void)
{
	if (host_config->card_detect.pin == 0xffU) {
		return true;
	}
	return sunxi_gpio_get(&host_config->card_detect) == 0;
}

/**
 * @brief Move the data sample point of the card interface.
 *
 * @details The phase goes into the new timing register and the delay into the sampling delay
 * register, which is enabled at the same time.
 *
 * @param[in] phase Sampling phase, 0 to 2.
 * @param[in] delay Sampling delay in the controller's delay units.
 */
void sunxi_mmc_tune_point(uint32_t phase, uint32_t delay)
{
	smhc_write((smhc_read(SMHC_NTSR) & ~NTSR_DAT_PH_MASK) | (phase << 8), SMHC_NTSR);
	smhc_write(SAMP_DL_ENABLE | (delay & SAMP_DL_MASK), SMHC_SAMP_DL);
}

/**
 * @brief Silence the log of expected failures during a tuning scan.
 *
 * @param[in] quiet true to suppress the per-transfer debug lines.
 */
void sunxi_mmc_set_quiet(bool quiet)
{
	host_quiet = quiet;
}

/**
 * @brief Report how many transfers used the DMA engine directly and how many bounced.
 *
 * @param[out] direct Number of direct transfers.
 * @param[out] bounce Number of bounced transfers.
 */
void sunxi_mmc_dma_stats(uint32_t *direct, uint32_t *bounce)
{
	*direct = dma_direct_count;
	*bounce = dma_bounce_count;
}

/* ---- the command engine ---- */

/**
 * @brief Wait until the card is not holding DAT0 low.
 *
 * @details A command may not be issued while the card is still busy with the previous one. The
 * wait polls the status register with a 10 us gap so the bus is not hammered.
 *
 * @param[in] timeout_ms Time limit in milliseconds.
 * @return 0 when the card is idle, or -EBUSY when it stayed busy.
 */
static int smhc_wait_card_idle(uint32_t timeout_ms)
{
	uint64_t deadline = ktime_get_ms() + timeout_ms;

	while ((smhc_read(SMHC_STAS) & STAS_CARD_BUSY) != 0U) {
		if (ktime_get_ms() > deadline) {
			return -EBUSY;
		}
		smhc_delay_us(10U);
	}
	return 0;
}

/**
 * @brief Move the data phase through the FIFO on the CPU.
 *
 * @details Moves one 32-bit word at a time, with a short tail when the length is not a multiple
 * of four. Each word waits for the FIFO to leave the opposite state: a write waits until it is
 * not full, a read until it is not empty. The error interrupts are checked on every pass, and
 * a timeout aborts. This is the path for small transfers (the tuning block, CMD6 status) and
 * the fallback when the DMA engine cannot be used.
 *
 * @param[in]  buffer     Data buffer.
 * @param[in]  length     Number of bytes.
 * @param[in]  write      true to write to the card.
 * @param[in]  timeout_ms Time limit in milliseconds.
 * @return 0 on success, -ETIMEDOUT when the deadline passed, or -EIO on an error interrupt.
 */
static int smhc_pio(uint8_t *buffer, uint32_t length, bool write, uint32_t timeout_ms)
{
	uint32_t done = 0U;
	uint64_t deadline = ktime_get_ms() + timeout_ms;

	while (done < length) {
		uint32_t word = 0U;
		uint32_t chunk = length - done < 4U ? length - done : 4U;

		if (ktime_get_ms() > deadline) {
			return -ETIMEDOUT;
		}
		{
			uint32_t interrupts = smhc_read(SMHC_RINTR);

			if ((interrupts & INT_ERRORS) != 0U) {
				if (!host_quiet) {
					pr_debug("data (%u, %s): rint %08x stas %08x at %u\n", (uint32_t)length,
						write ? "write" : "read", (uint32_t)interrupts,
						(uint32_t)smhc_read(SMHC_STAS), (uint32_t)done);
				}
				return -EIO;
			}
		}
		if (write) {
			if ((smhc_read(SMHC_STAS) & STAS_FIFO_FULL) != 0U) {
				continue;
			}
			memcpy(&word, buffer + done, chunk);
			smhc_write(word, SMHC_FIFO);
		} else {
			if ((smhc_read(SMHC_STAS) & STAS_FIFO_EMPTY) != 0U) {
				continue;
			}
			word = smhc_read(SMHC_FIFO);
			memcpy(buffer + done, &word, chunk);
		}
		done += chunk;
	}
	return 0;
}

/**
 * @brief Set the FIFO thresholds for a data phase.
 *
 * @details A fast card fills the FIFO faster than the CPU drains it. With the read threshold
 * the controller waits for a whole block before it lets the FIFO be read, and a write starts
 * only once the block fits. The threshold is only used when the block is small enough for the
 * field and fits the FIFO with the watermark.
 *
 * @param[in] block_size Size of one block in bytes.
 * @param[in] write      true for a write, false for a read.
 */
static void smhc_set_data_threshold(uint32_t block_size, bool write)
{
	uint32_t threshold = smhc_read(SMHC_THLD) & ~(THLD_SIZE_MASK | THLD_RD_EN | THLD_WR_EN);

	if (block_size <= 0xfffU) {
		if (write && block_size <= TX_WATERMARK_BYTES) {
			threshold |= (block_size << 16) | THLD_WR_EN;
		} else if (!write && host_fast && block_size + RX_WATERMARK_BYTES <= FIFO_BYTES) {
			threshold |= (block_size << 16) | THLD_RD_EN;
		}
	}
	smhc_write(threshold, SMHC_THLD);
}

/**
 * @brief Build the DMA descriptor list for one transfer.
 *
 * @details Splits the length into chunks of at most DESC_MAX_BYTES, marks the first and last
 * entries, chains the others and lets the engine own every entry. Addresses are stored divided
 * by four as the engine expects. The list lives in normal cacheable memory, so it is cleaned
 * to the point of coherency before the engine is started.
 *
 * @param[in] buffer Data buffer the descriptors point at.
 * @param[in] length Number of bytes.
 * @return 0 on success, or -EINVAL when the length needs more descriptors than exist.
 */
static int smhc_build_descriptors(void *buffer, uint32_t length)
{
	uint32_t count = (length + DESC_MAX_BYTES - 1U) / DESC_MAX_BYTES;

	if (count == 0U || count > DESC_COUNT) {
		return -EINVAL;
	}
	for (uint32_t index = 0U; index < count; index++) {
		uint32_t chunk = MIN(length - index * DESC_MAX_BYTES, DESC_MAX_BYTES);
		uint32_t config = DESC_CH | DESC_OWN;

		if (index == 0U) {
			config |= DESC_FD;
		}
		if (index + 1U == count) {
			config |= DESC_LD | DESC_ER;
		} else {
			config |= DESC_DIC;
		}
		dma_descriptors[index].config = config;
		dma_descriptors[index].size = chunk;
		dma_descriptors[index].buffer_address =
			((uintptr_t)buffer + index * DESC_MAX_BYTES) >> DESC_ADDRESS_SHIFT;
		dma_descriptors[index].next_address = index + 1U < count ?
							      ((uintptr_t)&dma_descriptors[index + 1U]) >>
								      DESC_ADDRESS_SHIFT :
							      0U;
	}
	dcache_clean_range(dma_descriptors, count * sizeof(dma_descriptors[0]));
	return 0;
}

/**
 * @brief Start the internal DMA engine on the descriptor list.
 *
 * @details Resets first the controller's DMA and FIFO paths and then the engine itself, and
 * waits for both resets to clear, because a start from an unknown state moves data to the
 * wrong place. The engine then runs with fixed bursts and the descriptor list base is written
 * shifted by two. The receive interrupt is enabled for reads only; the burst size and the
 * receive and transmit watermarks are programmed in the FIFO control register.
 *
 * @param[in] read true for a transfer from the card.
 */
static void smhc_dma_start(bool read)
{
	uint32_t gctrl = smhc_read(SMHC_GCTRL) & ~GCTRL_ACCESS_BY_AHB;

	smhc_write(gctrl | GCTRL_DMA_EN, SMHC_GCTRL);
	smhc_write(gctrl | GCTRL_DMA_EN | GCTRL_DMA_RST | GCTRL_FIFO_RST, SMHC_GCTRL);
	(void)smhc_wait_clear(SMHC_GCTRL, GCTRL_DMA_RST | GCTRL_FIFO_RST, 100U);
	smhc_write(DMAC_SOFT_RST, SMHC_DMAC);
	(void)smhc_wait_clear(SMHC_DMAC, DMAC_SOFT_RST, 100U);
	smhc_write(DMAC_FIX_BURST | DMAC_ON, SMHC_DMAC);
	smhc_write(read ? IDST_RX_INT : 0U, SMHC_IDIE);
	smhc_write((uint32_t)((uintptr_t)dma_descriptors >> DESC_ADDRESS_SHIFT), SMHC_DLBA);
	smhc_write((DMA_BURST_SIZE << 28) | (DMA_RX_WATERMARK << 16) | DMA_TX_WATERMARK, SMHC_FTRGL);
}

/**
 * @brief Stop the internal DMA engine and clear its state.
 *
 * @details Clears the DMA status and interrupt enable before switching the engine off, then
 * resets the DMA path and waits for the reset. Called on every exit path of a DMA transfer,
 * including failures, so the next command starts from a known state.
 */
static void smhc_dma_stop(void)
{
	smhc_write(0xffffffffU, SMHC_IDST);
	smhc_write(0U, SMHC_IDIE);
	smhc_write(0U, SMHC_DMAC);
	smhc_write((smhc_read(SMHC_GCTRL) | GCTRL_DMA_RST) & ~GCTRL_DMA_EN, SMHC_GCTRL);
	(void)smhc_wait_clear(SMHC_GCTRL, GCTRL_DMA_RST, 100U);
}

/**
 * @brief Run one command, optionally with a data phase.
 *
 * @details Builds the command register from the response shape: R2 also takes the long
 * response form and a CRC check, R3 takes neither, and the others check the CRC. CMD0 carries
 * the initialisation sequence and CMD11 the voltage-switch flag, which makes the controller
 * run that handshake itself.
 *
 * The function then waits for the card to release DAT0, clears the pending interrupts and,
 * for a data command, programs the block size and count and the FIFO threshold. The data path
 * is picked here: a transfer between 512 bytes and the bounce buffer size goes through the
 * internal DMA engine when the buffer and length are cache line aligned, and through the
 * bounce buffer otherwise (a write is copied in first and a read copied out at the end);
 * anything else, and any transfer the descriptor list cannot describe, falls back to the FIFO
 * driven by the CPU. A multi-block transfer arms the automatic stop command and waits for both
 * its completion and the data end.
 *
 * Completion is polled, not interrupt driven: a command without data waits for its own
 * interrupt, a data command additionally waits for the DMA receive interrupt. An error bit in
 * either interrupt register stops the DMA engine, records a description for the error UI and
 * returns a timeout or an I/O error. The response registers are read back on success and the
 * data cache is invalidated for a DMA read.
 *
 * @param[in]  opcode        Command index, six bits.
 * @param[in]  argument      Command argument.
 * @param[in]  response_kind Expected response shape.
 * @param[out] response      Four words receiving the response.
 * @param[in,out] buffer     Data buffer, or NULL for a command without data.
 * @param[in]  blocks        Number of blocks in the data phase.
 * @param[in]  block_size    Bytes per block.
 * @param[in]  write         true to write to the card, false to read.
 * @param[in]  timeout_ms    Extra time on top of the built-in one second.
 * @return 0 on success, -ETIMEDOUT when the card or the engine did not finish in time, or -EIO
 *         for any other error the controller reported.
 */
int sunxi_mmc_command(uint32_t opcode, uint32_t argument, enum mmc_response response_kind,
	uint32_t response[4], void *buffer, uint32_t blocks, uint32_t block_size, bool write,
	uint32_t timeout_ms)
{
	uint32_t cmdr = CMDR_START | (opcode & CMDR_OPCODE_MASK);
	uint32_t needed = INT_CMD_DONE;
	uint32_t length = 0U;
	void *dma_buffer = NULL;
	uint64_t deadline;
	uint32_t rint;
	int result;

	for (uint32_t word = 0U; word < 4U; word++) {
		response[word] = 0U;
	}
	/* the response shape decides the long-response bit and the CRC check */
	switch (response_kind) {
	case MMC_RESP_NONE:
		break;
	case MMC_RESP_R2:
		cmdr |= CMDR_RSP_EXP | CMDR_LONG_RSP | CMDR_CHECK_CRC;
		break;
	case MMC_RESP_R3:
		cmdr |= CMDR_RSP_EXP;
		break;
	default:
		cmdr |= CMDR_RSP_EXP | CMDR_CHECK_CRC;
		break;
	}
	if (opcode == 0U) { /* GO_IDLE_STATE */
		cmdr |= CMDR_SEND_INIT_SEQ;
	}
	if (opcode == 11U) { /* SWITCH_VOLTAGE: the controller runs the handshake itself */
		cmdr |= CMDR_VOLT_SWITCH;
		needed |= INT_VOLT_DONE;
	}
	result = smhc_wait_card_idle(200U);
	if (result != 0) {
		mmc_note_error("CMD%u Card Busy", (uint32_t)opcode);
		pr_err("CMD%u: card busy %d\n", (uint32_t)opcode, result);
		return result;
	}
	smhc_write(0xffffffffU, SMHC_RINTR);

	if (buffer != NULL) {
		length = blocks * block_size;
		smhc_write(block_size, SMHC_BLKSZ);
		smhc_write(length, SMHC_BCNTR);
		smhc_set_data_threshold(block_size, write);
		cmdr |= CMDR_DATA_EXP | CMDR_WAIT_PRE_OVER;
		if (write) {
			cmdr |= CMDR_WRITE;
		}
		if (blocks > 1U) {
			cmdr |= CMDR_AUTO_STOP;
			needed = INT_AUTO_CMD_DONE | INT_DATA_OVER;
		} else {
			needed = INT_DATA_OVER;
		}
		/*
		 * The DMA engine moves block sized transfers; it needs cache line aligned memory,
		 * so anything else is copied through the bounce buffer. Small transfers (registers,
		 * the tuning block) stay on the CPU path.
		 */
		if (length >= 512U && length <= DMA_BOUNCE_BYTES) {
			if (((uintptr_t)buffer % CACHE_LINE_BYTES) == 0U &&
			    (length % CACHE_LINE_BYTES) == 0U) {
				dma_buffer = buffer;
			} else {
				dma_buffer = dma_bounce;
				if (write) {
					memcpy(dma_buffer, buffer, length);
				}
			}
		}
		if (dma_buffer != NULL) {
			/* the engine reads the data and the list from memory: push both out */
			dcache_clean_range(dma_buffer, length);
			if (smhc_build_descriptors(dma_buffer, length) != 0) {
				dma_buffer = NULL;
			} else if (dma_buffer == buffer) {
				dma_direct_count++;
			} else {
				dma_bounce_count++;
			}
		}
		if (dma_buffer != NULL) {
			smhc_dma_start(!write);
		} else {
			/* FIFO through the AHB */
			smhc_write(smhc_read(SMHC_GCTRL) | GCTRL_ACCESS_BY_AHB | GCTRL_FIFO_RST, SMHC_GCTRL);
			(void)smhc_wait_clear(SMHC_GCTRL, GCTRL_FIFO_RST, 100U);
		}
	}
	/* no automatic stop for a single block, the full stop argument otherwise */
	smhc_write(blocks > 1U ? 0U : 0xffffU, SMHC_A12A);
	smhc_write(argument, SMHC_CARG);
	smhc_write(needed | INT_ERRORS, SMHC_IMASK);
	smhc_write(cmdr, SMHC_CMDR);

	if (buffer != NULL && dma_buffer == NULL) {
		/* the CPU moves the data while the command runs; the poll below then sees it done */
		result = smhc_pio(buffer, length, write, 1000U + timeout_ms);
		if (result != 0) {
			mmc_note_error("CMD%u %s %s", (uint32_t)opcode, write ? "Write" : "Read",
				result == -ETIMEDOUT ? "Timeout" : "Error");
			return result;
		}
	}
	deadline = ktime_get_ms() + 1000U + timeout_ms;
	for (;;) {
		uint32_t idst = dma_buffer != NULL ? smhc_read(SMHC_IDST) : 0U;

		rint = smhc_read(SMHC_RINTR);
		if ((rint & INT_ERRORS) != 0U || (idst & IDST_ERRORS) != 0U) {
			if (!host_quiet) {
				pr_debug("CMD%u (%u x %u): rint %08x idst %08x stas %08x\n",
					(uint32_t)opcode, (uint32_t)blocks, (uint32_t)block_size,
					(uint32_t)rint, (uint32_t)idst, (uint32_t)smhc_read(SMHC_STAS));
			}
			if (dma_buffer != NULL) {
				smhc_dma_stop();
			}
			{
				/* The UI shows this verbatim: say which command and which phase failed. */
				const char *direction = write ? "Write" : "Read";

				if ((rint & INT_RESP_TIMEOUT) != 0U) {
					mmc_note_error("CMD%u Response Timeout", (uint32_t)opcode);
				} else if ((rint & INT_DATA_TIMEOUT) != 0U) {
					mmc_note_error("CMD%u %s Timeout", (uint32_t)opcode, direction);
				} else if ((rint & INT_RESP_CRC_ERR) != 0U) {
					mmc_note_error("CMD%u Response CRC", (uint32_t)opcode);
				} else if ((rint & INT_DATA_CRC_ERR) != 0U) {
					mmc_note_error("CMD%u %s CRC", (uint32_t)opcode, direction);
				} else {
					mmc_note_error("CMD%u Error %08x", (uint32_t)opcode, (uint32_t)(rint | idst));
				}
			}
			return (rint & (INT_RESP_TIMEOUT | INT_DATA_TIMEOUT)) != 0U ? -ETIMEDOUT : -EIO;
		}
		/* a command only waits for its own bits; a DMA read also for the receive end */
		if (buffer == NULL) {
			if ((rint & needed) != 0U) {
				break;
			}
		} else if ((rint & needed) == needed &&
			   (dma_buffer == NULL || write || (idst & IDST_RX_INT) != 0U)) {
			break;
		}
		if (ktime_get_ms() > deadline) {
			if (dma_buffer != NULL) {
				smhc_dma_stop();
			}
			mmc_note_error("CMD%u %s Timeout", (uint32_t)opcode,
				buffer != NULL ? (write ? "Write" : "Read") : "Response");
			return -ETIMEDOUT;
		}
		if (dma_buffer != NULL) {
			/* the DMA engine is the one using the bus; do not poll it to death */
			smhc_delay_us(50U);
		}
	}
	smhc_write(rint, SMHC_RINTR);
	if (dma_buffer != NULL) {
		smhc_dma_stop();
		if (!write) {
			/* the engine wrote memory behind the cache's back: drop the stale lines */
			dcache_invalidate_range(dma_buffer, length);
			if (dma_buffer != buffer) {
				memcpy(buffer, dma_buffer, length);
			}
		}
	}
	for (uint32_t word = 0U; word < 4U; word++) {
		response[word] = smhc_read(SMHC_RESP0 + 4U * word);
	}
	return 0;
}

/**
 * @brief Reset the controller and restore the clock and bus width it had.
 *
 * @details Resets the command, FIFO and DMA paths in one write, waits for the resets to clear,
 * clears both status registers and the interrupt mask, sets the data timeout registers and
 * restores the width. The clock is then reprogrammed to the card clock in use, or to 400 kHz
 * when nothing has been set yet, which is what brings the card interface and the timing delays
 * back to a usable state.
 *
 * @return 0 on success, or the error from the clock update.
 */
int sunxi_mmc_recover(void)
{
	smhc_write(smhc_read(SMHC_GCTRL) | GCTRL_ALL_RST | GCTRL_INT_EN | GCTRL_ACCESS_DONE_DIR, SMHC_GCTRL);
	(void)smhc_wait_clear(SMHC_GCTRL, GCTRL_ALL_RST, 100U);
	smhc_write(0xffffffffU, SMHC_RINTR);
	smhc_write(0xffffffffU, SMHC_IDST);
	smhc_write(0xffffff00U | 0xffU, SMHC_TMOUT);
	smhc_write(0U, SMHC_IMASK);
	smhc_write(host_width == 4U ? 1U : host_width == 8U ? 2U : 0U, SMHC_WIDTH);
	return sunxi_mmc_set_clock(host_clock != 0U ? host_clock : 400000U);
}

/**
 * @brief Bring the controller to a known state and prepare the card port.
 *
 * @details Programs the port pins, enables the bus clock and releases the reset, then sets a
 * 400 kHz module clock, resets the controller, sets the data timeouts, masks the interrupts and
 * picks a one-bit bus, so the identification sequence can start.
 *
 * @param[in] config Board data for the slot.
 * @return 0 on success, -EINVAL for a NULL @p config or -ETIMEDOUT when the reset never clears.
 */
int sunxi_mmc_host_init(const struct sunxi_mmc_config *config)
{
	int result;

	if (config == NULL) {
		return -EINVAL;
	}
	host_config = config;
	/* the card port: clock, command, four data lines and the detect input */
	sunxi_pinctrl_set(&config->clock);
	sunxi_pinctrl_set(&config->command);
	for (uint32_t index = 0U; index < 4U; index++) {
		sunxi_pinctrl_set(&config->data[index]);
	}
	sunxi_pinctrl_set(&config->card_detect);
	sunxi_ccu_enable(config->gate_address, config->gate_bit, config->gate_address, config->reset_bit);
	smhc_set_module_clock(400000U);

	/* a known state: reset the controller, set the timeouts and 1-bit mode */
	smhc_write(smhc_read(SMHC_GCTRL) | GCTRL_ALL_RST | GCTRL_INT_EN | GCTRL_ACCESS_DONE_DIR, SMHC_GCTRL);
	result = smhc_wait_clear(SMHC_GCTRL, GCTRL_ALL_RST, 100U);
	if (result != 0) {
		pr_err("controller reset timed out\n");
		return result;
	}
	smhc_write(0xffffffffU, SMHC_RINTR);
	smhc_write(0xffffff00U | 0xffU, SMHC_TMOUT);
	smhc_write(0U, SMHC_IMASK);
	sunxi_mmc_set_width(1U);
	return 0;
}

/**
 * @brief Early init hook that prepares the board's slot.
 *
 * @return 0 on success, or the error from sunxi_mmc_host_init().
 */
static int mmc_host_init(void)
{
	return sunxi_mmc_host_init(&sunxi_mmc0);
}
INITCALL(2, 40, mmc_host_init);
