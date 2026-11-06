/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Newlib's allocator lock: one recursive mutex, created on first use. */

#include <reent.h>

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t malloc_mutex;

void __malloc_lock(struct _reent *reent);

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

void __malloc_unlock(struct _reent *reent)
{
	(void)reent;
	if (malloc_mutex != NULL) {
		(void)xSemaphoreGiveRecursive(malloc_mutex);
	}
}
