/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file sunxi-ccu.c
 * @brief The CCU as the drivers use it: one gate bit and one reset bit per module.
 *
 * Provides a single enable helper for modules that each own one clock gate
 * bit and one reset bit. Register addresses and bit positions come from the
 * board description, so the file contains no per-SoC tables.
 */

#include <hal/io.h>

#include "drivers/drivers.h"

/**
 * @brief Enable a module clock and bring the module out of reset.
 *
 * @details The sequence is assert, gate on, release. The reset bit is
 *          active low, so clearing it holds the module in reset while its
 *          clock is switched on; setting it afterwards lets the module start
 *          from a known state. Both registers are optional and are skipped
 *          when their address is zero, which allows clock-only or
 *          reset-only modules.
 *
 * @param[in] gate_address CCU gate register address, or 0 to leave the clock alone.
 * @param[in] gate_bit     Gate bit position within @p gate_address.
 * @param[in] reset_address CCU reset register address, or 0 to leave the reset alone.
 * @param[in] reset_bit    Reset bit position within @p reset_address.
 */
void sunxi_ccu_enable(uintptr_t gate_address, uint32_t gate_bit, uintptr_t reset_address,
	uint32_t reset_bit)
{
	if (reset_address != 0U) {
		clrbits32(reset_address, 1U << reset_bit); /* assert */
	}
	if (gate_address != 0U) {
		setbits32(gate_address, 1U << gate_bit);
	}
	if (reset_address != 0U) {
		setbits32(reset_address, 1U << reset_bit); /* release */
	}
}
