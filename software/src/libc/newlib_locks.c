/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file newlib_locks.c
 * @brief Newlib's allocator lock: one recursive mutex, created on first use.
 *
 * Newlib calls __malloc_lock() and __malloc_unlock() around every operation on its heap and
 * on stdio state, so malloc(), free() and the reentrant stdio functions are safe to call from
 * several tasks.
 */

#include <reent.h>

#include "FreeRTOS.h"
#include "semphr.h"

/** @brief Recursive mutex serialising newlib's allocator, created on first use. */
static SemaphoreHandle_t malloc_mutex;

void __malloc_lock(struct _reent *reent);

/**
 * @brief Take newlib's allocator lock, creating it the first time it is needed.
 *
 * The mutex is recursive so that a locked allocator function may call another one. Creation is
 * lazy and not itself locked; the first allocation is expected to happen before other tasks
 * are competing for the heap. The lock never times out.
 *
 * @param[in] reent Per-thread reentrancy structure; unused.
 */
void __malloc_lock(struct _reent *reent)
{
	(void)reent;
	if (malloc_mutex == NULL) {
		malloc_mutex = xSemaphoreCreateRecursiveMutex();
	}
	if (malloc_mutex != NULL) {
		(void)xSemaphoreTakeRecursive(malloc_mutex, portMAX_DELAY);
	}
}

void __malloc_unlock(struct _reent *reent);

/**
 * @brief Release newlib's allocator lock.
 *
 * Does nothing when the lock has not been created yet.
 *
 * @param[in] reent Per-thread reentrancy structure; unused.
 */
void __malloc_unlock(struct _reent *reent)
{
	(void)reent;
	if (malloc_mutex != NULL) {
		(void)xSemaphoreGiveRecursive(malloc_mutex);
	}
}
