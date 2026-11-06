/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Board data for the card slot: SMHC0 on port F, the card detect pin and the module clock. */

#ifndef CARDSIGHT_MMC_H
#define CARDSIGHT_MMC_H

#include <stdbool.h>
#include <stdint.h>

#include <board/cardsight_board.h>

struct sunxi_mmc_config {
	uintptr_t base;
	uintptr_t gate_address; /* CCU + 0x84c: bus clock gate, reset */
	uint8_t gate_bit;
	uint8_t reset_bit;
	uint32_t clock_register; /* CCU offset: 0x830 */
	uint32_t max_clock;
	uint32_t power_delay_ms;
	struct sunxi_pin clock;
	struct sunxi_pin command;
	struct sunxi_pin data[4];
	struct sunxi_pin card_detect;
};

extern const struct sunxi_mmc_config sunxi_mmc0;

#endif /* CARDSIGHT_MMC_H */
