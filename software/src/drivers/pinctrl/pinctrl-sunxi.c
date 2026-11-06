/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The pin controller of the SoC: per bank 0x30 bytes, CFG at +0x00 (4 bits per pin), DAT at
 * +0x10, DRV at +0x14 and PULL at +0x24 (2 bits per pin). Port B has no pins and port L sits at
 * its own offset. The drivers only need the function mux and a plain input read.
 */

#include <stdbool.h>

#include <hal/io.h>
#include <log.h>

#include "drivers/drivers.h"

#define REG_CFG	 0x00U
#define REG_DAT	 0x10U
#define REG_DRV	 0x14U
#define REG_PULL 0x24U

static int bank_offset(unsigned int port)
{
	if (port == 11U) { /* PL */
		return 0x540;
	}
	if (port > 7U || port == 1U) { /* PB is not bonded out */
		return -1;
	}
	return (int)(port * 0x30U);
}

void sunxi_pinctrl_set(const struct sunxi_pin *pin)
{
	uintptr_t bank;
	uint32_t shift;
	uintptr_t address;

	if (pin == NULL || (bank_offset(pin->port)) < 0) {
		return;
	}
	bank = SUNXI_GPIO_BASE + (uintptr_t)bank_offset(pin->port);
	address = bank + REG_CFG + (pin->pin / 8U) * 4U;
	shift = (pin->pin % 8U) * 4U;
	writel((readl(address) & ~(0xfU << shift)) | ((uint32_t)pin->function << shift), address);
	if (pin->pull != 0U) {
		address = bank + REG_PULL + (pin->pin / 16U) * 4U;
		shift = (pin->pin % 16U) * 2U;
		writel((readl(address) & ~(0x3U << shift)) | ((uint32_t)pin->pull << shift), address);
	}
	if (pin->drive != 0U) {
		address = bank + REG_DRV + (pin->pin / 8U) * 4U;
		shift = (pin->pin % 8U) * 4U;
		writel((readl(address) & ~(0xfU << shift)) | ((uint32_t)pin->drive << shift), address);
	}
}

void sunxi_gpio_set_output(const struct sunxi_pin *pin, int value)
{
	uintptr_t bank;
	uintptr_t address;
	uint32_t shift;

	if (pin == NULL || bank_offset(pin->port) < 0) {
		return;
	}
	bank = SUNXI_GPIO_BASE + (uintptr_t)bank_offset(pin->port);
	/* mux 1 is the plain output; without it the data bit goes nowhere */
	address = bank + REG_CFG + (pin->pin / 8U) * 4U;
	shift = (pin->pin % 8U) * 4U;
	writel((readl(address) & ~(0xfU << shift)) | (1U << shift), address);
	if (value != 0) {
		setbits32(bank + REG_DAT, 1U << pin->pin);
	} else {
		clrbits32(bank + REG_DAT, 1U << pin->pin);
	}
}

int sunxi_gpio_get(const struct sunxi_pin *pin)
{
	uintptr_t bank;

	if (pin == NULL || bank_offset(pin->port) < 0) {
		return -1;
	}
	bank = SUNXI_GPIO_BASE + (uintptr_t)bank_offset(pin->port);
	return (int)((readl(bank + REG_DAT) >> pin->pin) & 1U);
}
