// SPDX-License-Identifier: GPL-2.0-or-later
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

/* The one state the worker owns; modules take app_lock around every read and write of it. */
extern struct cardsight_state app;
extern struct mutex app_lock;
extern struct message_queue requests;

/* A refresh of the live values, posted to the LVGL task without waiting for it. */
void app_post_refresh_runtime(void);

/* Card.c: what the card currently holds, read again whenever the slot changes or a request ends. */
bool card_disk_present(void);
int card_scan(void);

/* Files.c: one directory of a partition, mounted temporarily unless the proxy already has it. */
int card_list_files(unsigned int partition, const char *path);

/* Format.c: one partition, quick (metadata only) or full (erase first). */
int format_partition(unsigned int partition, enum blk_partition_content content, bool full);

/* Layout.c: write the table, then format the partitions it made. */
int layout_apply(const struct cardsight_request *request);

#endif /* CARDSIGHT_APP_INTERNAL_H */
