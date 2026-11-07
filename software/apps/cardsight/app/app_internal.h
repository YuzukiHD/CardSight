// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file app_internal.h
 * @brief Shared state and module entry points of the application task.
 *
 * Only app.c owns the worker loop; the other modules run on that task and
 * touch the shared state under @c app_lock.
 */
#ifndef CARDSIGHT_APP_INTERNAL_H
#define CARDSIGHT_APP_INTERNAL_H

#include <errno.h>
#include <stdio.h>
#include <log.h>
#include "FreeRTOS.h"
#include <hal/blkdev.h>
#include <hal/kernel.h>
#include <hal/mmc/core.h>
#include <gui/lvgl_port.h>
#include <usb/mass_storage.h>

#include "app/app.h"
#include "storage/storage_proxy.h"
#include "ui/ui.h"

/** @brief Application state owned by the worker; modules hold @c app_lock for every access. */
extern struct cardsight_state app;
/** @brief Protects @c app against the UI thread and the diagnostic callbacks. */
extern struct mutex app_lock;
/** @brief Request queue between the UI and the worker, deep enough to absorb a burst of taps. */
extern struct message_queue requests;

/**
 * @brief Post a refresh of the live values to the LVGL task without waiting for it.
 */
void app_post_refresh_runtime(void);

/**
 * @brief Report whether a block device for the source disk can be opened.
 *
 * Card.c: what the card currently holds, read again whenever the slot changes or a request ends.
 *
 * @return True when the card is present and the source disk can be opened.
 */
bool card_disk_present(void);
/**
 * @brief Re-read the card capacity, table type, partitions and identity into @c app.
 *
 * @return 0 on success, or a negative errno from the partition scan.
 */
int card_scan(void);

/**
 * @brief List one directory of a partition into the shared file list.
 *
 * Files.c: one directory of a partition, mounted temporarily unless the proxy already has it.
 *
 * @param[in] partition Partition number, one-based.
 * @param[in] path      Directory path relative to the partition root.
 * @return 0; the listing's own error is stored with it instead of being returned.
 */
int card_list_files(uint32_t partition, const char *path);

/**
 * @brief Format one partition, quick (metadata only) or full (erase first).
 *
 * Format.c: one partition, quick (metadata only) or full (erase first).
 *
 * @param[in] partition Partition number, one-based.
 * @param[in] content   File system to create.
 * @param[in] full      True to zero the whole partition before writing the file system.
 * @return 0 on success, or a negative errno.
 */
int format_partition(uint32_t partition, enum blk_partition_content content, bool full);

/**
 * @brief Write the partition table, then format the partitions it made.
 *
 * Layout.c: write the table, then format the partitions it made.
 *
 * @param[in] request Layout request holding the table type and the partitions.
 * @return 0 on success, or a negative errno from the table write or a format.
 */
int layout_apply(const struct cardsight_request *request);


#endif /* CARDSIGHT_APP_INTERNAL_H */
