/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Capacity display in the decimal units a card is sold in: 1 kB is 1000 bytes, so a 32 GB
 * card reads "32 GB" rather than "29.7 GiB".
 */

#ifndef CARDSIGHT_SIZE_H
#define CARDSIGHT_SIZE_H

#include <stddef.h>
#include <stdint.h>

/** Write @p bytes into @p buffer as a rounded decimal capacity ("32 GB", "512 MB"). */
void format_capacity(uint64_t bytes, char *buffer, size_t size);

#endif /* CARDSIGHT_SIZE_H */
