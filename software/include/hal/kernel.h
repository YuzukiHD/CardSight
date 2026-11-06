/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What a driver or the application needs from the kernel: FreeRTOS itself, the small wrappers
 * above it (mutex, semaphore, message queue, delays) and the shared helpers.
 */

#ifndef CARDSIGHT_KERNEL_H
#define CARDSIGHT_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <bits.h>
#include <log.h>
#include <hal/delay.h>
#include <hal/ktime.h>
#include <hal/mutex.h>
#include <hal/message_queue.h>
#include <hal/semaphore.h>

/* Bytes the C library's bump allocator has handed out to malloc so far. */
size_t platform_heap_used(void);

#endif /* CARDSIGHT_KERNEL_H */
