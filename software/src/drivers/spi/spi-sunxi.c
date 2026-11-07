/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file spi-sunxi.c
 * @brief The sunxi SPI controller in master mode, single wire, mode 0: a
 *        polled FIFO writer with the sample delay line for the high clock.
 *
 * Only transmit is used on this board (the panel is write only), so the
 * receive path drains the FIFO and drops what it gets.
 */

#define LOG_MODULE "spi"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <hal/io.h>
#include <hal/kernel.h>
#include <log.h>

#include "arch/arch.h"
#include "drivers/drivers.h"
#include "drivers/spi/spi-sunxi.h"

/** @brief Global control register: enable, master mode, transmit pause. */
#define SPI_GC	     0x04U
/** @brief Transfer control register: clock mode, chip select and burst flags. */
#define SPI_TC	     0x08U
/** @brief Interrupt control register; left at zero (polled driver). */
#define SPI_INT_CTL  0x10U
/** @brief Interrupt status register; write 1 to clear a bit. */
#define SPI_INT_STA  0x14U
/** @brief FIFO control register: reset bits and fill thresholds. */
#define SPI_FIFO_CTL 0x18U
/** @brief FIFO status register: byte counts for TX and RX. */
#define SPI_FIFO_STA 0x1cU
/** @brief Clock control register: SCK divider and polarity settings. */
#define SPI_CLK_CTL  0x24U
/** @brief Master burst counter. */
#define SPI_BURST_CNT 0x30U
/** @brief Master transmit counter. */
#define SPI_TX_CNT   0x34U
/** @brief Burst control counter. */
#define SPI_BCC	     0x38U
/** @brief TX FIFO data port. */
#define SPI_TXDATA   0x200U
/** @brief RX FIFO data port. */
#define SPI_RXDATA   0x300U

/** @brief GC bit: enable the controller. */
#define GC_EN		  (1U << 0)
/** @brief GC bit: master mode. */
#define GC_MODE		  (1U << 1)
/** @brief GC bit: enable the transmit pause between bursts. */
#define GC_TP_EN	  (1U << 7)
/** @brief TC bit: chip-select polarity (1 = active low). */
#define TC_SPOL		  (1U << 2)
/** @brief TC bit: chip-select level when idle. */
#define TC_SS_LEVEL	  (1U << 7)
/** @brief TC bit: discard received data instead of storing it; write only, drop what comes back. */
#define TC_DHB		  (1U << 8)
/** @brief TC bit: discard the received data burst. */
#define TC_DDB		  (1U << 9)
/** @brief TC bit: sample-delay mode for the high clock. */
#define TC_SDM		  (1U << 13)
/** @brief TC bit: start the exchange. */
#define TC_XCH		  (1U << 31)
/** @brief Interrupt bits that report transfer errors (bits 8..10). */
#define INT_STA_ERRORS	  ((1U << 8) | (1U << 9) | (1U << 10))
/** @brief Interrupt bit: transfer complete. */
#define INT_STA_TC	  (1U << 12)
/** @brief FIFO control bit: reset the TX FIFO; self-clears when done. */
#define FIFO_TX_RST	  (1U << 31)
/** @brief FIFO control bit: reset the RX FIFO; self-clears when done. */
#define FIFO_RX_RST	  (1U << 15)
/** @brief FIFO depth in bytes (16 words of four bytes). */
#define FIFO_DEPTH	  64U
/** @brief Burst count field mask of @c SPI_BCC. */
#define BCC_STC		  0xffffffU
/** @brief Mask of the burst and transmit counters (24 bits). */
#define MAX_BURST	  0xffffffU

/** @brief Controller description set by @ref sunxi_spi_init; NULL until then. */
static const struct sunxi_spi_config *spi_config;

/**
 * @brief Poll a register until the given bits self-clear.
 *
 * @details Used for the FIFO reset bits, which the controller clears after it
 *          has emptied the corresponding FIFO.
 *
 * @param[in] offset Register offset from the controller base.
 * @param[in] bits   Mask of bits that must become zero.
 * @return 0 when all bits are clear, or -ETIMEDOUT after a fixed number of polls.
 */
static int spi_wait_clear(uint32_t offset, uint32_t bits)
{
	for (uint32_t spins = 0U; spins < 100000U; spins++) {
		if ((readl(spi_config->base + offset) & bits) == 0U) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/**
 * @brief Push a buffer through the TX FIFO and wait for the transfer to finish.
 *
 * @details Resets both FIFOs and waits for the reset bits to self-clear, then
 *          clears any stale interrupt status and programs the burst, transmit
 *          and burst-control counters with the byte count. The main loop then
 *          reads the FIFO status, which reports the TX byte count in bits
 *          16..23 and the RX byte count in bits 0..7. It fills the TX FIFO
 *          while keeping at least four bytes of room, packing four input
 *          bytes into one 32-bit store when possible because the FIFO is four
 *          bytes wide and the full-word store quarters the number of bus
 *          transactions; a tail shorter than four bytes is stored byte by
 *          byte. The exchange is started once the first bytes are queued, and
 *          the transmit pause feature lets the FIFO refill during the burst.
 *          Received bytes are drained and dropped. Error interrupt bits abort
 *          with -EIO, the transfer-complete bit returns success, and the
 *          timer-tick deadline guards against a stalled controller. The
 *          counters are masked to 24 bits, so a buffer longer than 0xffffff
 *          bytes cannot be sent as a single transfer.
 *
 * @param[in] data     Bytes to send.
 * @param[in] length   Number of bytes to send.
 * @param[in] deadline Absolute CLINT tick count after which the transfer is
 *                     abandoned.
 * @return 0 on success, -EIO on a controller error, or -ETIMEDOUT when the
 *         deadline passes.
 */
static int spi_send(const uint8_t *data, size_t length, uint64_t deadline)
{
	uintptr_t base = spi_config->base;
	uint32_t fifo_ctl = readl(base + SPI_FIFO_CTL);
	size_t written = 0U;
	bool started = false;

	fifo_ctl |= FIFO_TX_RST | FIFO_RX_RST;
	writel(fifo_ctl, base + SPI_FIFO_CTL);
	if (spi_wait_clear(SPI_FIFO_CTL, FIFO_TX_RST | FIFO_RX_RST) != 0) {
		return -ETIMEDOUT;
	}
	writel(0xffffffffU, base + SPI_INT_STA);
	writel((uint32_t)length & MAX_BURST, base + SPI_BURST_CNT);
	writel((uint32_t)length & MAX_BURST, base + SPI_TX_CNT);
	clrsetbits32(base + SPI_BCC, BCC_STC, (uint32_t)length & BCC_STC);

	/* keep the FIFO ahead of the wire; the deadline is in timer ticks (no division in the loop) */
	for (;;) {
		uint32_t status = readl(base + SPI_FIFO_STA);
		uint32_t count = (status >> 16) & 0xffU;
		uint32_t received = status & 0xffU;

		/* one store pushes four bytes, as the FIFO is four bytes wide: a quarter of the
		 * bus transactions, which matters when the other hart is drawing */
		while (count < FIFO_DEPTH - 4U && written < length) {
			if (written + 4U <= length) {
				const uint8_t *bytes = data + written;
				uint32_t packed = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
					((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);

				writel(packed, base + SPI_TXDATA);
				written += 4U;
				count += 4U;
			} else {
				writeb(data[written], base + SPI_TXDATA);
				written++;
				count++;
			}
		}
		/* start as soon as the FIFO holds the first bytes; the transmit pause lets it refill */
		if (!started && written != 0U) {
			clrsetbits32(base + SPI_TC, TC_XCH, TC_XCH);
			started = true;
		}
		while (received-- != 0U) {
			(void)readb(base + SPI_RXDATA);
		}
		{
			uint32_t interrupts = readl(base + SPI_INT_STA);

			if ((interrupts & INT_STA_ERRORS) != 0U) {
				writel(0xffffffffU, base + SPI_INT_STA);
				return -EIO;
			}
			if (started && (interrupts & INT_STA_TC) != 0U) {
				writel(0xffffffffU, base + SPI_INT_STA);
				return 0;
			}
		}
		if (cs_clint_ticks() > deadline) {
			pr_err("%u bytes: wrote %u, fifo %08x int %08x tc %08x\n", (uint32_t)length,
				(uint32_t)written, (uint32_t)readl(base + SPI_FIFO_STA),
				(uint32_t)readl(base + SPI_INT_STA), (uint32_t)readl(base + SPI_TC));
			return -ETIMEDOUT;
		}
	}
}

/**
 * @brief Send a buffer on MOSI with the controller driving the chip select.
 *
 * @details Derives a one-second deadline from the CLINT frequency and hands
 *          the transfer to @ref spi_send. The controller holds no per-transfer
 *          state beyond the configuration, but concurrent callers would still
 *          interleave on the bus, so callers must serialise transfers.
 *
 * @param[in] buffer Bytes to send.
 * @param[in] length Number of bytes to send.
 * @return 0 on success, -EINVAL for bad arguments, -EIO on a controller
 *         error, or -ETIMEDOUT when the transfer stalls.
 */
int sunxi_spi_write(const void *buffer, size_t length)
{
	uint64_t deadline;

	if (spi_config == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	deadline = cs_clint_ticks() + cs_clint_frequency();
	return spi_send(buffer, length, deadline);
}

/**
 * @brief Initialise the SPI controller for mode 0, single-wire transmit.
 *
 * @details Stores the board description, muxes SCK, MOSI and CS, and enables
 *          the bus clock and reset through the CCU helper. The module clock
 *          register is then reprogrammed: bit 31 selects the peripheral PLL
 *          (300 MHz, mux 1) over the 24 MHz crystal (mux 0), bit 24 selects
 *          that parent, and the low bits hold a divider clamped to 1..16 that
 *          produces the requested SCK; a frequency of 0 defaults to 60 MHz.
 *          The SCK divider, clock phase and clock polarity fields of
 *          @c SPI_CLK_CTL are cleared so the module clock drives SCK directly.
 *          Global control enables the controller, master mode and the
 *          transmit pause. Transfer control selects mode 0, chip select 0
 *          active low and held high when idle, drops received data and uses
 *          the sample-delay mode; the panel only takes writes. Interrupts stay
 *          polled, all status bits are cleared, and the FIFOs are reset with
 *          TX/RX thresholds of 48 and 32 bytes. Initialisation fails if the
 *          FIFO reset never completes.
 *
 * @param[in] config Board description of the controller.
 * @return 0 on success, -EINVAL when @p config is NULL, or -ETIMEDOUT when
 *         the FIFO reset times out.
 */
int sunxi_spi_init(const struct sunxi_spi_config *config)
{
	uint32_t module;

	if (config == NULL) {
		return -EINVAL;
	}
	spi_config = config;

	sunxi_pinctrl_set(&config->clock);
	sunxi_pinctrl_set(&config->mosi);
	sunxi_pinctrl_set(&config->cs);
	sunxi_ccu_enable(config->gate_address, config->gate_bit, config->gate_address, config->reset_bit);

	/*
	 * The module clock feeds SCK directly (the controller divider stays at 1). Mux 1 is the
	 * 300 MHz output of the peripheral PLL, mux 0 the 24 MHz crystal.
	 */
	{
		uint32_t divider = 300000000U / (config->frequency != 0U ? config->frequency : 60000000U);

		divider = divider < 1U ? 1U : divider > 16U ? 16U : divider;
		module = (1U << 31) | (1U << 24) | (divider - 1U);
	}
	writel(module, SUNXI_CCU_BASE + config->module_clock_register);
	writel((readl(config->base + SPI_CLK_CTL) & ~((0xfU << 8) | (1U << 12) | 0xffU)),
		config->base + SPI_CLK_CTL);

	writel(GC_EN | GC_MODE | GC_TP_EN, config->base + SPI_GC);
	/* mode 0, chip select 0 active low, sample delay mode; the panel only takes writes */
	writel(TC_SS_LEVEL | TC_SPOL | TC_DHB | TC_DDB | TC_SDM | (0U << 4), config->base + SPI_TC);
	writel(0U, config->base + SPI_INT_CTL);
	writel(0xffffffffU, config->base + SPI_INT_STA);
	{
		uint32_t fifo = readl(config->base + SPI_FIFO_CTL) | FIFO_TX_RST | FIFO_RX_RST;

		fifo &= ~((0xffU << 16) | 0xffU);
		fifo |= (48U << 16) | 32U;
		writel(fifo, config->base + SPI_FIFO_CTL);
		if (spi_wait_clear(SPI_FIFO_CTL, FIFO_TX_RST | FIFO_RX_RST) != 0) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}
