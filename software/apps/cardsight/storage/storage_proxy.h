/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CARDSIGHT_STORAGE_PROXY_H
#define CARDSIGHT_STORAGE_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

#define STORAGE_PROXY_MAX_PARTITIONS 8U

struct storage_proxy_config {
	const char *source_disk;
	uint32_t selected_partitions; /* bit zero is partition one */
};

struct storage_proxy_partition_info {
	char device[VFS_BLK_NAME];
	struct vfs_volume_info volume;
	uint64_t sectors;
	uint32_t partno;
	int status;
	bool present;
	bool selected;
	bool writable;
};

int storage_proxy_start(const char *source_disk);
int storage_proxy_start_config(const struct storage_proxy_config *config);
int storage_proxy_scan(const char *source_disk, struct storage_proxy_partition_info *partitions,
	size_t capacity, size_t *count);
void storage_proxy_stop(void);
void storage_proxy_source_lost(void);
bool storage_proxy_running(void);
bool storage_proxy_read_only(void);
const char *storage_proxy_device_name(void);
size_t storage_proxy_entry_count(void);
int storage_proxy_partition_status(uint32_t partition);

#endif /* CARDSIGHT_STORAGE_PROXY_H */
