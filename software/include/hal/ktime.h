/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file ktime.h
 * @brief A finer clock for timings: microseconds, from the same tick.
 */

#ifndef CARDSIGHT_KTIME_H
#define CARDSIGHT_KTIME_H

#include <stdint.h>

/**
 * @brief Return the wall clock in microseconds since boot.
 *
 * @details Converts the difference between the current cycle counter and its value at
 * initialization to microseconds.
 *
 * @return Microseconds since boot.
 */
uint64_t ktime_get_us(void);

#endif /* CARDSIGHT_KTIME_H */
