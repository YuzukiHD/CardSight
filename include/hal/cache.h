/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Data cache maintenance for DMA. The C907 has the T-Head cache maintenance operations, which
 * work on physical addresses; the range operations round outwards to whole cache lines.
 */

#ifndef CARDSIGHT_CACHE_H
#define CARDSIGHT_CACHE_H

#include <stddef.h>

#define CACHE_LINE_BYTES 64U

/* Write the dirty lines of the range back to memory. */
void dcache_clean_range(const void *address, size_t length);

/* Drop the cached copy of the range; data written to memory by a device is read back. */
void dcache_invalidate_range(const void *address, size_t length);

#endif /* CARDSIGHT_CACHE_H */
