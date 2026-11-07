/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file card.h
 * @brief The card as the application sees it: what it is, how it is accessed and what the host
 * found.
 *
 * The structure is owned by the mmc core; get a reference with mmc_host_card_get().
 */

#ifndef CARDSIGHT_MMC_CARD_H
#define CARDSIGHT_MMC_CARD_H

#include <stdbool.h>
#include <stdint.h>
#include <hal/device.h>
#include <hal/mmc/cid.h>

struct mmc_host;

/**
 * @brief What the host found out about the card.
 *
 * The trailing fields describe what the card offers, read during identification.
 */
struct mmc_card_info {
	uint32_t sectors; /**< Capacity in 512 byte sectors. */
	uint32_t clock;	  /**< Clock in use, in Hz. */
	int bus_width; /**< Data bus width in bits. */
	bool high_capacity; /**< SDHC/SDXC, which use block addressing. */
	uint8_t cid[16]; /**< Raw card identification register. */
	struct mmc_cid_identity identity; /**< Decoded card identification. */
	uint16_t rca; /**< Relative card address. */
	bool s18a; /**< The card accepted 1.8 V signalling in ACMD41. */
	uint16_t sw_groups[6]; /**< CMD6 support masks of function groups 1..6. */
	uint16_t max_current;  /**< Maximum current in mA, from CMD6 status. */
	const char *mode; /**< Speed mode in use: SDR104, SDR50, DDR50, SDR25, default. */
	bool tuned;	       /**< The sample point was found by tuning. */
};

/**
 * @brief A card known to the mmc core.
 */
struct mmc_card {
	struct device dev; /**< Device whose reference is released by put_device(). */
	struct mmc_host *host; /**< Host the card is attached to. */
	struct mmc_card_info info; /**< What the host found out. */
	bool ready; /**< Whether identification succeeded and the card is present. */
};

#endif /* CARDSIGHT_MMC_CARD_H */
