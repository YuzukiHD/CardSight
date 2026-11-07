/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file size.h
 * @brief Capacity display in the decimal units a card is sold in: 1 kB is 1000 bytes, so a
 * 32 GB card reads "32 GB" rather than "29.7 GiB".
 */

#ifndef CARDSIGHT_SIZE_H
#define CARDSIGHT_SIZE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Write a decimal capacity into a buffer.
 *
 * @details Picks TB, GB, MB, kB or B from the magnitude and rounds to the nearest whole unit,
 * so a 32 GB card reads "32 GB" and a 512 MB one "512 MB".
 *
 * @param[in]  bytes  Capacity in bytes.
 * @param[out] buffer Destination string.
 * @param[in]  size   Size of @p buffer in bytes; nothing is written when zero.
 */
void format_capacity(uint64_t bytes, char *buffer, size_t size);

#endif /* CARDSIGHT_SIZE_H */
