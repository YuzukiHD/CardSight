/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SD/MMC core: the description of the card and the disk it creates ("mmcblk0").
 *
 * Return values are 0 or a negative errno. A card reference keeps the structure alive after the
 * card went away; its description stays readable.
 */

#ifndef CARDSIGHT_MMC_CORE_H
#define CARDSIGHT_MMC_CORE_H

#include <hal/mmc/card.h>
#include <hal/mmc/host.h>

/** The card was identified and has not been removed. */
bool mmc_card_ready(const struct mmc_card *card);

/** Copy what the card reports; -ENODEV without a ready card. */
int mmc_card_info(const struct mmc_card *card, struct mmc_card_info *info);

/** Register the disk driver: the card becomes the block device "mmcblk0". */
int mmc_blk_init(void);

/** The last command-level card failure, e.g. "CMD5 Read Timeout"; empty before one. */
const char *mmc_last_error(void);
void mmc_last_error_clear(void);

/** The last identification result: negative while a card in the slot cannot be brought up. */
int mmc_probe_error(void);

/** A card is in the slot but has not been identified yet. */
bool mmc_slot_detecting(void);

#endif /* CARDSIGHT_MMC_CORE_H */
