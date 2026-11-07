/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file storage_proxy.h
 * @brief Public interface of the exFAT proxy view.
 *
 * The proxy synthesises one exFAT volume whose partition folders are backed by
 * the mounted source file systems, and exposes it as block device
 * @c cardview0 for the USB mass storage gadget.
 */
#ifndef CARDSIGHT_STORAGE_PROXY_H
#define CARDSIGHT_STORAGE_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

/** @brief Number of source partitions the view can expose. */
#define STORAGE_PROXY_MAX_PARTITIONS 8U

/**
 * @brief What the caller wants the proxy to expose.
 */
struct storage_proxy_config {
	/** @brief Block device name of the whole source disk. */
	const char *source_disk;
	/** @brief Bit mask of partitions to expose; bit zero is partition one. */
	uint32_t selected_partitions; /* bit zero is partition one */
};

/**
 * @brief One source partition as found by a scan of the card.
 */
struct storage_proxy_partition_info {
	/** @brief Block device name of the partition, such as "mmc0p1". */
	char device[VFS_BLK_NAME];
	/** @brief Volume information probed from the partition. */
	struct vfs_volume_info volume;
	/** @brief Size of the partition in 512-byte sectors. */
	uint64_t sectors;
	/** @brief One-based partition number. */
	uint32_t partno;
	/** @brief Result of probing the file system, or -ENODEV when it is absent. */
	int status;
	/** @brief True when the partition device exists. */
	bool present;
	/** @brief True when the partition is in the proxy's selection mask. */
	bool selected;
	/** @brief True when the partition can be written. */
	bool writable;
};

/**
 * @brief Start the view over every partition found on the disk.
 *
 * @param[in] source_disk Block device name of the whole disk.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_start(const char *source_disk);
/**
 * @brief Start the view with an explicit partition selection.
 *
 * @param[in] config Source disk and selection mask.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_start_config(const struct storage_proxy_config *config);
/**
 * @brief Describe the partitions of a disk without starting the view.
 *
 * @param[in]  source_disk Whole-disk block device name.
 * @param[out] partitions  Array receiving one entry per present partition.
 * @param[in]  capacity    Number of entries @p partitions can hold.
 * @param[out] count       Number of entries filled.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_scan(const char *source_disk, struct storage_proxy_partition_info *partitions,
	size_t capacity, size_t *count);
/**
 * @brief Stop the view and release everything it holds.
 */
void storage_proxy_stop(void);
/**
 * @brief Mark the source card as gone so the view only fails until it is stopped.
 */
void storage_proxy_source_lost(void);
/**
 * @brief Report whether the view is running.
 *
 * @return True when the view is registered as a block device.
 */
bool storage_proxy_running(void);
/**
 * @brief Report whether the view refuses writes.
 *
 * @return True when any exposed source is read-only.
 */
bool storage_proxy_read_only(void);
/**
 * @brief Return the block device name the view is registered under.
 *
 * @return The constant device name of the view.
 */
const char *storage_proxy_device_name(void);
/**
 * @brief Return the number of index nodes the view currently holds.
 *
 * @return Node count, including the root and retired nodes.
 */
size_t storage_proxy_entry_count(void);
/**
 * @brief Return the probe status of one source partition.
 *
 * @param[in] partition Zero-based partition index.
 * @return The stored status, or -EINVAL when the index is out of range.
 */
int storage_proxy_partition_status(uint32_t partition);

#endif /* CARDSIGHT_STORAGE_PROXY_H */
