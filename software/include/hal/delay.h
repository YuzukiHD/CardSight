/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file delay.h
 * @brief Delays and the wall clock: the kernel tick counts milliseconds from the boot.
 */

#ifndef CARDSIGHT_DELAY_H
#define CARDSIGHT_DELAY_H

#include <stdint.h>

/**
 * @brief Sleep for a number of milliseconds.
 *
 * @details Once the scheduler runs this yields the CPU through the kernel delay; before that it
 * busy-waits on the cycle counter, so early init code can call it too.
 *
 * @param[in] milliseconds Delay length in milliseconds.
 */
void msleep(uint32_t milliseconds);

/**
 * @brief Milliseconds since the scheduler started.
 *
 * @return The wall clock in milliseconds.
 */
uint64_t ktime_get_ms(void);

#endif /* CARDSIGHT_DELAY_H */
