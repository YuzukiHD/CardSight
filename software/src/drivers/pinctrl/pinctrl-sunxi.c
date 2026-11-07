/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file pinctrl-sunxi.c
 * @brief Pin mux, pull, drive and GPIO data access for the SoC.
 *
 * Each bank spans 0x30 bytes: CFG at +0x00 (4 bits per pin), DAT at +0x10,
 * DRV at +0x14 and PULL at +0x24 (2 bits per pin). Port B has no pins and
 * port L sits at its own offset. The drivers only need the function mux and a
 * plain input read.
 */

#include <stdbool.h>

#include <hal/io.h>
#include <log.h>

#include "drivers/drivers.h"

/** @brief Offset of the configuration register within a bank. */
#define REG_CFG	 0x00U
/** @brief Offset of the data register within a bank. */
#define REG_DAT	 0x10U
/** @brief Offset of the drive-strength register within a bank. */
#define REG_DRV	 0x14U
/** @brief Offset of the pull register within a bank. */
#define REG_PULL 0x24U

/**
 * @brief Translate a port letter into its register block offset.
 *
 * @details Ports 0..7 map linearly to 0x30-byte blocks. Port L (11) is not
 *          part of that run and has a fixed offset of 0x540. Port B (1) is not
 *          bonded out, and ports above 7 other than L have no block; both
 *          cases return -1 so callers can ignore the pin.
 *
 * @param[in] port Port index, 0 for PA, 1 for PB, and so on.
 * @return The block offset in bytes, or -1 when the port has no pins.
 */
static int bank_offset(uint32_t port)
{
	if (port == 11U) { /* PL */
		return 0x540;
	}
	if (port > 7U || port == 1U) { /* PB is not bonded out */
		return -1;
	}
	return (int)(port * 0x30U);
}

/**
 * @brief Mux a pin and, when requested, set its pull and drive strength.
 *
 * @details The configuration register holds eight 4-bit function fields, so
 *          the word is selected by @c pin/8 and the field by @c pin%8. Pull
 *          uses 2-bit fields packed sixteen per word and drive uses 4-bit
 *          fields like CFG. Each write is a read-modify-write that clears and
 *          replaces only the pin's field. Pull and drive are skipped when
 *          their value in the description is zero, leaving the reset value.
 *          Pins on a bank without pins are ignored.
 *
 * @param[in] pin Pin description to apply.
 */
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

/**
 * @brief Drive a GPIO pin as a plain output at the requested level.
 *
 * @details Forces the pin's function field to mux 1 (plain output) because
 *          the data bit has no effect in any other mux, then sets or clears
 *          the corresponding bit in the bank's data register. Pins on a bank
 *          without pins are ignored.
 *
 * @param[in] pin   Pin description.
 * @param[in] value Non-zero for high, zero for low.
 */
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

/**
 * @brief Read the current level of a pin from its bank's data register.
 *
 * @details Returns the raw data-register bit regardless of the selected mux,
 *          which lets callers sample an input pin without changing its
 *          function.
 *
 * @param[in] pin Pin description.
 * @return The sampled level (0 or 1), or -1 when the pin's bank has no pins.
 */
int sunxi_gpio_get(const struct sunxi_pin *pin)
{
	uintptr_t bank;

	if (pin == NULL || bank_offset(pin->port) < 0) {
		return -1;
	}
	bank = SUNXI_GPIO_BASE + (uintptr_t)bank_offset(pin->port);
	return (int)((readl(bank + REG_DAT) >> pin->pin) & 1U);
}
