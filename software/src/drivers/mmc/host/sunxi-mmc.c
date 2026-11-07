/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SMHC0, the SD/MMC host controller: clocks, bus width, the polled command engine with its
 * internal DMA, the data sample point and the I/O supply switch. The SD protocol on top of it
 * lives in core/sd.c; this file knows commands only as opcodes and response shapes.
 */

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

#define SMHC_GCTRL  0x00U
#define SMHC_CLKCR  0x04U
#define SMHC_TMOUT  0x08U
#define SMHC_WIDTH  0x0cU
#define SMHC_BLKSZ  0x10U
#define SMHC_BCNTR  0x14U
#define SMHC_CMDR   0x18U
#define SMHC_CARG   0x1cU
#define SMHC_RESP0  0x20U
#define SMHC_IMASK  0x30U
#define SMHC_RINTR  0x38U
#define SMHC_STAS   0x3cU
#define SMHC_FTRGL  0x40U
#define SMHC_CSDC   0x54U
#define SMHC_A12A   0x58U
#define SMHC_NTSR   0x5cU
#define SMHC_DMAC   0x80U
#define SMHC_DLBA   0x84U
#define SMHC_IDST   0x88U
#define SMHC_IDIE   0x8cU
#define SMHC_THLD   0x100U
#define SMHC_DRV_DL 0x140U
#define SMHC_SAMP_DL 0x144U
#define SMHC_FIFO   0x200U

#define GCTRL_SOFT_RST	      (1U << 0)
#define GCTRL_FIFO_RST	      (1U << 1)
#define GCTRL_DMA_RST	      (1U << 2)
#define GCTRL_ALL_RST	      (GCTRL_SOFT_RST | GCTRL_FIFO_RST | GCTRL_DMA_RST)
#define GCTRL_INT_EN	      (1U << 4)
#define GCTRL_DMA_EN	      (1U << 5)
#define GCTRL_ACCESS_DONE_DIR (1U << 30)
#define GCTRL_ACCESS_BY_AHB   (1U << 31)

#define CLKCR_DIV_MASK	 0xffU
#define CLKCR_CARD_CLK_ON (1U << 16)
#define CLKCR_MASK_DATA0  (1U << 31)

#define CMDR_OPCODE_MASK 0x3fU
#define CMDR_RSP_EXP	 (1U << 6)
#define CMDR_LONG_RSP	 (1U << 7)
#define CMDR_CHECK_CRC	 (1U << 8)
#define CMDR_DATA_EXP	 (1U << 9)
#define CMDR_WRITE	 (1U << 10)
#define CMDR_AUTO_STOP	 (1U << 12)
#define CMDR_WAIT_PRE_OVER (1U << 13)
#define CMDR_SEND_INIT_SEQ (1U << 15)
#define CMDR_UPCLK_ONLY	 (1U << 21)
#define CMDR_VOLT_SWITCH (1U << 28)
#define CMDR_START	 (1U << 31)

#define INT_RESP_ERR	  (1U << 1)
#define INT_CMD_DONE	  (1U << 2)
#define INT_DATA_OVER	  (1U << 3)
#define INT_RESP_CRC_ERR  (1U << 6)
#define INT_DATA_CRC_ERR  (1U << 7)
#define INT_RESP_TIMEOUT  (1U << 8)
#define INT_DATA_TIMEOUT  (1U << 9)
#define INT_VOLT_DONE	  (1U << 10)
#define INT_FIFO_RUN_ERR  (1U << 11)
#define INT_HARD_LOCKED	  (1U << 12)
#define INT_START_BIT_ERR (1U << 13)
#define INT_AUTO_CMD_DONE (1U << 14)
#define INT_END_BIT_ERR	  (1U << 15)
#define INT_ERRORS                                                                                 \
	(INT_RESP_ERR | INT_RESP_CRC_ERR | INT_DATA_CRC_ERR | INT_RESP_TIMEOUT | INT_DATA_TIMEOUT | \
	 INT_FIFO_RUN_ERR | INT_HARD_LOCKED | INT_START_BIT_ERR | INT_END_BIT_ERR)

#define STAS_FIFO_EMPTY (1U << 2)
#define STAS_FIFO_FULL	(1U << 3)
#define STAS_CARD_BUSY	(1U << 9)

/* new timing mode: sample phases, drive phases and the sampling delay */
#define NTSR_2X_TIMING	    (1U << 31)
#define NTSR_DAT_PH_MASK    (3U << 8)
#define DRV_DL_CMD_PH	    (1U << 16)
#define DRV_DL_DAT_PH	    (1U << 17)
#define SAMP_DL_ENABLE	    (1U << 7)
#define SAMP_DL_MASK	    0x3fU
#define CSDC_CRC_STATUS_DETECT 3U
#define CSDC_CRC_MASK	    0xfU

/* the FIFO holds 1 KiB; a fast read waits for a whole block before the CPU drains it */
#define FIFO_BYTES	    1024U
#define RX_WATERMARK_BYTES  28U
#define TX_WATERMARK_BYTES  992U
#define THLD_RD_EN	    (1U << 0)
#define THLD_WR_EN	    (1U << 2)
#define THLD_SIZE_MASK	    (0xfffU << 16)

/* the internal DMA controller and its descriptor list */
#define DMAC_SOFT_RST  (1U << 0)
#define DMAC_FIX_BURST (1U << 1)
#define DMAC_ON	       (1U << 7)
#define DESC_DIC       (1U << 1)
#define DESC_LD	       (1U << 2)
#define DESC_FD	       (1U << 3)
#define DESC_CH	       (1U << 4)
#define DESC_ER	       (1U << 5)
#define DESC_OWN       (1U << 31)
#define DESC_MAX_BYTES 8192U
#define DESC_COUNT     16U /* 128 KiB, the largest block command */
#define DESC_ADDRESS_SHIFT 2U
#define IDST_RX_INT    (1U << 1)
#define IDST_ERRORS    ((1U << 2) | (1U << 4) | (1U << 5) | (1U << 9))
#define DMA_BURST_SIZE 2U
#define DMA_RX_WATERMARK 7U
#define DMA_TX_WATERMARK 248U
#define DMA_BOUNCE_BYTES (DESC_COUNT * DESC_MAX_BYTES)

/* the card port's supply switch inside the chip: 1 is 3.3 V, 0 is 1.8 V, bit 5 reports it */
#define PAD_VOLTAGE_CONTROL (SUNXI_GPIO_BASE + 0x390U)
#define PAD_VOLTAGE_STATUS  (SUNXI_GPIO_BASE + 0x38cU)
#define PAD_VOLTAGE_MASK    (1U << 5)

/* ---- the controller's state ---- */

static const struct sunxi_mmc_config *host_config;
static uint32_t host_clock;
static uint32_t host_width = 1U;
static bool host_fast; /* SDR50/SDR104: the read FIFO needs a whole-block threshold */
static bool host_quiet; /* a tuning scan expects the failed reads */
static uint32_t dma_direct_count;
static uint32_t dma_bounce_count;

struct smhc_descriptor {
	uint32_t config;
	uint32_t size;
	uint32_t buffer_address;
	uint32_t next_address;
};

static struct smhc_descriptor dma_descriptors[DESC_COUNT] __attribute__((aligned(CACHE_LINE_BYTES)));
static uint8_t dma_bounce[DMA_BOUNCE_BYTES] __attribute__((aligned(CACHE_LINE_BYTES)));

/* ---- register helpers ---- */

static uint32_t smhc_read(uint32_t offset)
{
	return readl(host_config->base + offset);
}

static void smhc_write(uint32_t value, uint32_t offset)
{
	writel(value, host_config->base + offset);
}

static void smhc_delay_us(uint32_t microseconds)
{
	cs_udelay(microseconds);
}

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

/* module clock: HOSC 24 MHz (mux 0) below 12 MHz, the 400 MHz peripheral PLL above */
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

/*
 * Stop or start the card clock; the controller latches the change on its own command. During
 * the 1.8 V sequence the command carries the voltage switch flag so the controller takes part.
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

int sunxi_mmc_set_clock(uint32_t clock)
{
	uint32_t clkcr = smhc_read(SMHC_CLKCR) & ~(CLKCR_CARD_CLK_ON | CLKCR_DIV_MASK);
	uint32_t drive;
	uint32_t timing;
	uint32_t sample = 0U;
	int result = smhc_clock_switch(false, false);

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

void sunxi_mmc_set_width(uint32_t width)
{
	host_width = width;
	smhc_write(width == 4U ? 1U : width == 8U ? 2U : 0U, SMHC_WIDTH);
}

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

bool sunxi_mmc_card_busy(void)
{
	return (smhc_read(SMHC_STAS) & STAS_CARD_BUSY) != 0U;
}

bool sunxi_mmc_present(void)
{
	if (host_config->card_detect.pin == 0xffU) {
		return true;
	}
	return sunxi_gpio_get(&host_config->card_detect) == 0;
}

void sunxi_mmc_tune_point(uint32_t phase, uint32_t delay)
{
	smhc_write((smhc_read(SMHC_NTSR) & ~NTSR_DAT_PH_MASK) | (phase << 8), SMHC_NTSR);
	smhc_write(SAMP_DL_ENABLE | (delay & SAMP_DL_MASK), SMHC_SAMP_DL);
}

void sunxi_mmc_set_quiet(bool quiet)
{
	host_quiet = quiet;
}

void sunxi_mmc_dma_stats(uint32_t *direct, uint32_t *bounce)
{
	*direct = dma_direct_count;
	*bounce = dma_bounce_count;
}

/* ---- the command engine ---- */

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

/*
 * A fast card fills the FIFO faster than the CPU drains it. With the read threshold the
 * controller waits for a whole block before it lets the FIFO be read, and a write starts only
 * once the block fits.
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

static void smhc_dma_stop(void)
{
	smhc_write(0xffffffffU, SMHC_IDST);
	smhc_write(0U, SMHC_IDIE);
	smhc_write(0U, SMHC_DMAC);
	smhc_write((smhc_read(SMHC_GCTRL) | GCTRL_DMA_RST) & ~GCTRL_DMA_EN, SMHC_GCTRL);
	(void)smhc_wait_clear(SMHC_GCTRL, GCTRL_DMA_RST, 100U);
}

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
	smhc_write(blocks > 1U ? 0U : 0xffffU, SMHC_A12A);
	smhc_write(argument, SMHC_CARG);
	smhc_write(needed | INT_ERRORS, SMHC_IMASK);
	smhc_write(cmdr, SMHC_CMDR);

	if (buffer != NULL && dma_buffer == NULL) {
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

int sunxi_mmc_host_init(const struct sunxi_mmc_config *config)
{
	int result;

	if (config == NULL) {
		return -EINVAL;
	}
	host_config = config;
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

static int mmc_host_init(void)
{
	return sunxi_mmc_host_init(&sunxi_mmc0);
}
INITCALL(2, 40, mmc_host_init);
