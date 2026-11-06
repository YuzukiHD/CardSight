/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host stand-in for hal/kernel.h: the UI only needs the shared helpers and a sleeping stub. */
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

static inline void msleep(uint32_t milliseconds)
{
	(void)milliseconds;
}

#endif
