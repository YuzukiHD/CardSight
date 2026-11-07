/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file host.h
 * @brief The cards of the board.
 *
 * The host is a singleton here: CardSight has one SD slot, index 0.
 */

#ifndef CARDSIGHT_MMC_HOST_H
#define CARDSIGHT_MMC_HOST_H

#include <hal/mmc/card.h>

/** @brief One SD/MMC controller instance. */
struct mmc_host;

/**
 * @brief Return the host at an index.
 *
 * @param[in] index Host index; the board has only index 0.
 * @return The host, or NULL when the board has none at @p index.
 */
struct mmc_host *mmc_host_get(uint32_t index);

/**
 * @brief Return the card in a host with a reference.
 *
 * @param[in] host Host to query.
 * @return The card, which the caller releases with put_device(&card->dev), or NULL when the
 * slot is empty.
 */
struct mmc_card *mmc_host_card_get(struct mmc_host *host);

#endif /* CARDSIGHT_MMC_HOST_H */
