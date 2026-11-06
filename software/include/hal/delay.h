/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Delays and the wall clock: the kernel tick counts milliseconds from the boot.
 */

#ifndef CARDSIGHT_DELAY_H
#define CARDSIGHT_DELAY_H

#include <stdint.h>

void msleep(uint32_t milliseconds);

/** Milliseconds since the scheduler started. */
uint64_t ktime_get_ms(void);

#endif /* CARDSIGHT_DELAY_H */
