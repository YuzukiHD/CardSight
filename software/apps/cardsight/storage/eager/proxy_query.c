// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_query.c
 * @brief Queries of the running view: state, partitions and device name.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/**
 * @brief Report whether the view is running.
 *
 * @return True when the view is registered as a block device.
 */
bool storage_proxy_running(void)
{
	return proxy.running;
}


/**
 * @brief Report whether the view refuses writes.
 *
 * @return True when any exposed source is read-only.
 */
bool storage_proxy_read_only(void)
{
	return proxy.read_only;
}


/**
 * @brief Describe the partitions of a disk without starting the view.
 *
 * @details The source must be a whole disk. Each "diskN" partition is opened;
 * a missing one is skipped without leaving a hole, so the output array is
 * packed. The volume is probed and the partition is only reported writable
 * when the probe succeeded and neither the device nor the volume is read-only.
 *
 * @param[in]  source_disk Whole-disk block device name.
 * @param[out] partitions  Array receiving one entry per present partition.
 * @param[in]  capacity    Number of entries @p partitions can hold.
 * @param[out] count       Number of entries filled.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_scan(const char *source_disk, struct storage_proxy_partition_info *partitions,
	size_t capacity, size_t *count)
{
	struct block_device *source;
	size_t found = 0U;

	if (source_disk == NULL || partitions == NULL || count == NULL) {
		return -EINVAL;
	}
	source = bdev_open_by_name(source_disk);
	if (source == NULL || !bdev_is_whole(source)) {
		if (source != NULL) {
			bdev_close(source);
		}
		return -ENODEV;
	}
	bdev_close(source);
	for (uint32_t index = 0U; index < PROXY_PARTITIONS && found < capacity; index++) {
		struct storage_proxy_partition_info *info = &partitions[found];
		struct block_device *bdev;

		memset(info, 0, sizeof(*info));
		snprintf(info->device, sizeof(info->device), "%sp%u", source_disk, index + 1U);
		bdev = bdev_open_by_name(info->device);
		if (bdev == NULL) {
			continue;
		}
		info->present = true;
		info->partno = index + 1U;
		info->sectors = bdev_nr_sectors(bdev);
		info->writable = !bdev_read_only(bdev);
		bdev_close(bdev);
		info->status = vfs_probe(info->device, &info->volume);
		info->writable = info->status == 0 && info->writable && !info->volume.read_only;
		found++;
	}
	*count = found;
	return 0;
}


/**
 * @brief Return the block device name the view is registered under.
 *
 * @return The constant device name of the view.
 */
const char *storage_proxy_device_name(void)
{
	return PROXY_DISK_NAME;
}


/**
 * @brief Return the number of index nodes the view currently holds.
 *
 * @return Node count, including the root and retired nodes.
 */
size_t storage_proxy_entry_count(void)
{
	return proxy.node_count;
}


/**
 * @brief Return the probe status of one source partition.
 *
 * @param[in] partition Zero-based partition index.
 * @return The stored status, or -EINVAL when the index is out of range.
 */
int storage_proxy_partition_status(uint32_t partition)
{
	return partition < PROXY_PARTITIONS ? proxy.partitions[partition].status : -EINVAL;
}
