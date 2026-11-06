/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The VFS lock on FreeRTOS: a recursive mutex, because a file system may reach back into the VFS
 * (the storage proxy reads its source files while the host reads the view).
 */

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t vfs_mutex;

void vfs_os_lock(void)
{
	if (vfs_mutex == NULL) {
		vfs_mutex = xSemaphoreCreateRecursiveMutex();
	}
	if (vfs_mutex != NULL) {
		(void)xSemaphoreTakeRecursive(vfs_mutex, portMAX_DELAY);
	}
}

void vfs_os_unlock(void)
{
	if (vfs_mutex != NULL) {
		(void)xSemaphoreGiveRecursive(vfs_mutex);
	}
}
