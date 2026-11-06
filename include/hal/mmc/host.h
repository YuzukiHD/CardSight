/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The cards of the board. The host is a singleton here: CardSight has one SD slot, index 0.
 */

#ifndef CARDSIGHT_MMC_HOST_H
#define CARDSIGHT_MMC_HOST_H

#include <hal/mmc/card.h>

struct mmc_host;

/** The host at @p index, or NULL when the board has none. */
struct mmc_host *mmc_host_get(unsigned int index);

/** The card in @p host with a reference (put_device(&card->dev)), or NULL when the slot is empty. */
struct mmc_card *mmc_host_card_get(struct mmc_host *host);

#endif /* CARDSIGHT_MMC_HOST_H */
