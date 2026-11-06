// SPDX-License-Identifier: GPL-2.0-or-later
/* Writing a new partition table and formatting what it created. */

#include "app/app_internal.h"


/* Every explicit size plus the 1 MiB alignment of each partition must leave room for the rest. */
static bool layout_fits(const struct cardsight_request *request, uint64_t card_sectors)
{
	uint64_t needed = (uint64_t)(request->count + 1U) * 2048U + 2048U;

	if (request->count == 0U || request->count > APP_LAYOUT_MAX ||
		(request->table == BLK_PARTITION_TABLE_MBR && request->count > 4U)) {
		return false;
	}
	for (uint32_t index = 0U; index < request->count; index++) {
		needed += request->entries[index].sectors;
	}
	return needed <= card_sectors;
}


int layout_apply(const struct cardsight_request *request)
{
	struct block_device *bdev = bdev_open_by_name(CONFIG_STORAGE_PROXY_SOURCE_DISK);
	int result;

	if (bdev == NULL || !bdev_is_whole(bdev)) {
		if (bdev != NULL) {
			bdev_close(bdev);
		}
		return -ENODEV;
	}
	if (!layout_fits(request, bdev_nr_sectors(bdev))) {
		bdev_close(bdev);
		return -ENOSPC;
	}
	result = blk_write_partition_layout(bdev->bd_disk, request->table, request->entries, request->count);
	bdev_close(bdev);
	for (uint32_t index = 0U; result == 0 && index < request->count; index++) {
		result = format_partition(index + 1U, request->entries[index].content, false);
	}
	return result;
}
