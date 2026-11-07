/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file kernel.h
 * @brief Host stand-in for hal/kernel.h: the UI only needs the shared helpers
 *        and a sleeping stub.
 *
 * Sleeps return immediately so the simulated UI runs at full host speed
 * without blocking the test.
 */
#ifndef CARDSIGHT_HOST_HAL_KERNEL_H
#define CARDSIGHT_HOST_HAL_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <bits.h>
#include <log.h>

/**
 * @brief Sleep for a number of milliseconds.
 *
 * @param[in] milliseconds Requested delay; ignored on the host.
 */
static inline void msleep(uint32_t milliseconds)
{
	(void)milliseconds;
}

#endif
