/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file core.h
 * @brief SD/MMC core: the description of the card and the disk it creates ("mmcblk0").
 *
 * Return values are 0 or a negative errno. A card reference keeps the structure alive after the
 * card went away; its description stays readable.
 */

#ifndef CARDSIGHT_MMC_CORE_H
#define CARDSIGHT_MMC_CORE_H

#include <hal/mmc/card.h>
#include <hal/mmc/host.h>

/**
 * @brief Whether the card was identified and has not been removed.
 *
 * @param[in] card Card to check; NULL is not ready.
 * @return true for the live, identified card.
 */
bool mmc_card_ready(const struct mmc_card *card);

/**
 * @brief Copy what the card reports.
 *
 * @param[in]  card Card to query.
 * @param[out] info Receives a snapshot of the card description.
 * @return 0 on success, or -ENODEV without a ready card.
 */
int mmc_card_info(const struct mmc_card *card, struct mmc_card_info *info);

/**
 * @brief Register the disk driver.
 *
 * @details Initializes the card lock, waits for the slot power delay, probes a card when one is
 * present, and registers the card as the block device "mmcblk0".
 *
 * @return 0 on success, a negative errno when the probe fails, or 0 when the slot is empty.
 */
int mmc_blk_init(void);

/**
 * @brief The last command-level card failure.
 *
 * @return A message such as "CMD5 Read Timeout", or an empty string before one.
 */
const char *mmc_last_error(void);
/** @brief Forget the last command-level failure. */
void mmc_last_error_clear(void);

/**
 * @brief The last identification result.
 *
 * @return 0 after a good probe, or a negative errno while a card in the slot cannot be brought
 * up.
 */
int mmc_probe_error(void);

/**
 * @brief Whether a card is in the slot but has not been identified yet.
 *
 * @return true while a present card is still being brought up.
 */
bool mmc_slot_detecting(void);

#endif /* CARDSIGHT_MMC_CORE_H */
