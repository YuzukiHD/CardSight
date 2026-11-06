/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The CCU as the drivers use it: one gate bit and one reset bit per module. */

#include <hal/io.h>

#include "drivers/drivers.h"

void sunxi_ccu_enable(uintptr_t gate_address, unsigned int gate_bit, uintptr_t reset_address,
	unsigned int reset_bit)
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
