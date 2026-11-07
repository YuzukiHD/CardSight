/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file sunxi-mmc.h
 * @brief Board data for the card slot: SMHC0 on port F, the card detect pin and the module clock.
 */

#ifndef CARDSIGHT_MMC_H
#define CARDSIGHT_MMC_H

#include <stdbool.h>
#include <stdint.h>

#include <board/cardsight_board.h>

/**
 * @brief Description of one MMC host controller instance on the board.
 */
struct sunxi_mmc_config {
	/** @brief MMIO base of the SMHC registers. */
	uintptr_t base;
	/** @brief CCU + 0x84c: bus clock gate, reset. */
	uintptr_t gate_address;
	/** @brief Bit of @c gate_address that gates the bus clock. */
	uint8_t gate_bit;
	/** @brief Bit of @c gate_address that resets the controller. */
	uint8_t reset_bit;
	/** @brief CCU offset of the module clock, 0x830. */
	uint32_t clock_register;
	/** @brief Highest card clock the board supports in Hz. */
	uint32_t max_clock;
	/** @brief Time to wait after power-up before probing in milliseconds. */
	uint32_t power_delay_ms;
	/** @brief Pin of the card clock output. */
	struct sunxi_pin clock;
	/** @brief Pin of the command line. */
	struct sunxi_pin command;
	/** @brief Pins of data lines DAT0..DAT3. */
	struct sunxi_pin data[4];
	/** @brief Card detect pin; pin number 0xff means the slot has none. */
	struct sunxi_pin card_detect;
};

/** @brief The board's only slot: SMHC0 on port F. */
extern const struct sunxi_mmc_config sunxi_mmc0;

#endif /* CARDSIGHT_MMC_H */
