/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file vfs_os.c
 * @brief The VFS lock on FreeRTOS: a recursive mutex.
 *
 * It is recursive because a file system may reach back into the VFS (the storage proxy reads
 * its source files while the host reads the view).
 */

#include "FreeRTOS.h"
#include "semphr.h"

/** @brief Recursive mutex that serialises VFS calls; created on first use. */
static SemaphoreHandle_t vfs_mutex;

/**
 * @brief Take the VFS lock, creating the mutex on first use.
 *
 * When the mutex cannot be created the call returns without locking, so a system that runs
 * out of kernel memory degrades to the old unprotected behaviour instead of hanging.
 */
void vfs_os_lock(void)
{
	if (vfs_mutex == NULL) {
		vfs_mutex = xSemaphoreCreateRecursiveMutex();
	}
	if (vfs_mutex != NULL) {
		(void)xSemaphoreTakeRecursive(vfs_mutex, portMAX_DELAY);
	}
}

/**
 * @brief Release the VFS lock.
 *
 * Does nothing while the mutex does not exist, matching vfs_os_lock().
 */
void vfs_os_unlock(void)
{
	if (vfs_mutex != NULL) {
		(void)xSemaphoreGiveRecursive(vfs_mutex);
	}
}
