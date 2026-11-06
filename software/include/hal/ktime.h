/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A finer clock for timings: microseconds, from the same tick.
 */

#ifndef CARDSIGHT_KTIME_H
#define CARDSIGHT_KTIME_H

#include <stdint.h>

uint64_t ktime_get_us(void);

#endif /* CARDSIGHT_KTIME_H */
