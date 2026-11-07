// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file layout.c
 * @brief Writing a new partition table and formatting what it created.
 */

#include "app/app_internal.h"


/**
 * @brief Check that a layout is well formed and fits the card.
 *
 * @details Every explicit size plus the 1 MiB alignment of each partition must
 * leave room for the rest. The estimate charges a 1 MiB slice for each
 * partition entry, one for the table itself, and one spare slice, then rejects
 * an empty layout, one too large for the request array, and an MBR layout with
 * more than four entries (MBR has no room for them).
 *
 * @param[in] request     Layout request to check.
 * @param[in] card_sectors Capacity of the card in 512-byte sectors.
 * @return True when the layout can be written.
 */
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


/**
 * @brief Write a new partition table and format the partitions it created.
 *
 * @details Only a whole disk can be re-partitioned, and the layout is rejected
 * before anything is written when it does not fit. The old table is cleared
 * first: it removes a stale GPT backup and any table the new one would not
 * cover, so the card really starts from a clean one. After the new table is
 * written, each partition is formatted in turn as a quick format, and the
 * first failure stops the sequence. The device is closed before formatting so
 * the partition devices can be opened.
 *
 * @param[in] request Layout request holding the table type and the partitions.
 * @return 0 on success, or a negative errno from the table write or a format.
 */
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
	/* Clear the old table first: it removes a stale GPT backup and any table the new
	 * one would not cover, so the card really starts from a clean one. */
	result = blk_erase_partition_table(bdev->bd_disk);
	if (result == 0) {
		result = blk_write_partition_layout(bdev->bd_disk, request->table, request->entries,
			request->count);
	}
	bdev_close(bdev);
	for (uint32_t index = 0U; result == 0 && index < request->count; index++) {
		result = format_partition(index + 1U, request->entries[index].content, false);
	}
	return result;
}
