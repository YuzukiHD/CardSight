/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file cache.h
 * @brief Data cache maintenance for DMA.
 *
 * The C907 has the T-Head cache maintenance operations, which work on physical addresses; the
 * range operations round outwards to whole cache lines.
 */

#ifndef CARDSIGHT_CACHE_H
#define CARDSIGHT_CACHE_H

#include <stddef.h>

/** @brief Size of one data cache line in bytes. */
#define CACHE_LINE_BYTES 64U

/**
 * @brief Write the dirty lines of the range back to memory.
 *
 * @details The start is rounded down and the end up to whole cache lines; the range is fenced
 * before and after the T-Head clean operations.
 *
 * @param[in] address Physical start address of the range.
 * @param[in] length  Length in bytes; zero does nothing.
 */
void dcache_clean_range(const void *address, size_t length);

/**
 * @brief Drop the cached copy of the range.
 *
 * @details Data written to memory by a device is read back on the next access. The range is
 * rounded outwards to whole cache lines and fenced before and after the T-Head invalidate
 * operations.
 *
 * @param[in] address Physical start address of the range.
 * @param[in] length  Length in bytes; zero does nothing.
 */
void dcache_invalidate_range(const void *address, size_t length);

#endif /* CARDSIGHT_CACHE_H */
