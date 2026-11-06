/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The sunxi SPI controller in master mode, single wire, mode 0: a polled FIFO writer with the
 * sample delay line for the high clock. Only transmit is used on this board (the panel is
 * write only), so the receive path drains the FIFO and drops what it gets.
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

#define SPI_GC	     0x04U
#define SPI_TC	     0x08U
#define SPI_INT_CTL  0x10U
#define SPI_INT_STA  0x14U
#define SPI_FIFO_CTL 0x18U
#define SPI_FIFO_STA 0x1cU
#define SPI_CLK_CTL  0x24U
#define SPI_BURST_CNT 0x30U
#define SPI_TX_CNT   0x34U
#define SPI_BCC	     0x38U
#define SPI_TXDATA   0x200U
#define SPI_RXDATA   0x300U

#define GC_EN		  (1U << 0)
#define GC_MODE		  (1U << 1)
#define GC_TP_EN	  (1U << 7)
#define TC_SPOL		  (1U << 2)
#define TC_SS_LEVEL	  (1U << 7)
#define TC_DHB		  (1U << 8) /* write only: drop what comes back */
#define TC_DDB		  (1U << 9)
#define TC_SDM		  (1U << 13)
#define TC_XCH		  (1U << 31)
#define INT_STA_ERRORS	  ((1U << 8) | (1U << 9) | (1U << 10))
#define INT_STA_TC	  (1U << 12)
#define FIFO_TX_RST	  (1U << 31)
#define FIFO_RX_RST	  (1U << 15)
#define FIFO_DEPTH	  64U
#define BCC_STC		  0xffffffU
#define MAX_BURST	  0xffffffU

static const struct sunxi_spi_config *spi_config;

static int spi_wait_clear(uint32_t offset, uint32_t bits)
{
	for (unsigned int spins = 0U; spins < 100000U; spins++) {
		if ((readl(spi_config->base + offset) & bits) == 0U) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

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

		while (count < FIFO_DEPTH - 4U && written < length) {
			writeb(data[written], base + SPI_TXDATA);
			written++;
			count++;
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
			pr_err("%u bytes: wrote %u, fifo %08x int %08x tc %08x\n", (unsigned)length,
				(unsigned)written, (unsigned)readl(base + SPI_FIFO_STA),
				(unsigned)readl(base + SPI_INT_STA), (unsigned)readl(base + SPI_TC));
			return -ETIMEDOUT;
		}
	}
}

int sunxi_spi_write(const void *buffer, size_t length)
{
	uint64_t deadline;

	if (spi_config == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	deadline = cs_clint_ticks() + cs_clint_frequency();
	return spi_send(buffer, length, deadline);
}

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
