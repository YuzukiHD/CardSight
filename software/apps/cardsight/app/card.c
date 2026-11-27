// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file card.c
 * @brief Reading the card: what is present, its table type, its partitions and its identity.
 */

#include "app/app_private.h"


/**
 * @brief Report whether the source disk is present.
 *
 * @details Presence is tested by opening the block device by name and closing
 * it again, which is the same path the rest of the application uses.
 *
 * @return True when the source disk can be opened.
 */
bool card_disk_present(void)
{
	struct block_device *disk = bdev_open_by_name(CONFIG_STORAGE_PROXY_SOURCE_DISK);

	if (disk == NULL) {
		return false;
	}
	bdev_close(disk);
	return true;
}


/**
 * @brief Identify the partition table type from the first sectors of the disk.
 *
 * @details The MBR signature at offset 510 is required. A protective 0xEE
 * partition type in the first MBR slot plus the "EFI PART" signature at LBA 1
 * means GPT; anything else is treated as MBR so an unreadable table is shown
 * as MBR rather than refused.
 *
 * @param[in] bdev Whole-disk block device to read.
 * @return Table type found on the disk.
 */
static enum blk_partition_table_type detect_table(struct block_device *bdev)
{
	uint8_t sector[512] __attribute__((aligned(64)));

	if (bdev_read(bdev, 0U, sector, 1U) == 0 && sector[510] == 0x55U && sector[511] == 0xaaU &&
		sector[450] == 0xeeU && bdev_read(bdev, 1U, sector, 1U) == 0 && memcmp(sector, "EFI PART", 8U) == 0) {
		return BLK_PARTITION_TABLE_GPT;
	}
	return BLK_PARTITION_TABLE_MBR;
}


/**
 * @brief Re-read the card capacity, table type, partitions and identity into the shared state.
 *
 * @details The quick properties (presence, capacity, writability, table type)
 * are published under @c app_lock right after the device is opened. The slower
 * partition probe and the MMC identity query then run without the lock, and
 * their results are published in a second critical section only when they
 * succeeded, so a temporary failure does not erase the last good values.
 *
 * @return 0 on success, or a negative errno from the partition scan.
 */
int card_scan(void)
{
	struct block_device *disk = bdev_open_by_name(CONFIG_STORAGE_PROXY_SOURCE_DISK);
	struct storage_proxy_partition_info partitions[STORAGE_PROXY_MAX_PARTITIONS];
	struct mmc_card_info card_info;
	struct mmc_host *host;
	struct mmc_card *card;
	size_t count = 0U;
	bool card_info_valid = false;
	int result;

	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	app.card_present = disk != NULL;
	app.card_info_valid = false;
	app.partition_count = 0U;
	app.sectors = disk != NULL ? bdev_nr_sectors(disk) : 0U;
	app.card_writable = disk != NULL && !bdev_read_only(disk);
	app.table = disk != NULL ? detect_table(disk) : BLK_PARTITION_TABLE_MBR;
	mutex_unlock(&app_lock);
	if (disk != NULL) {
		bdev_close(disk);
	}
	result = storage_proxy_scan(CONFIG_STORAGE_PROXY_SOURCE_DISK, partitions, ARRAY_SIZE(partitions), &count);
	host = mmc_host_get(0U);
	card = host != NULL ? mmc_host_card_get(host) : NULL;
	if (card != NULL && mmc_card_info(card, &card_info) == 0) {
		card_info_valid = true;
	}
	/* mmc_host_card_get() takes a device reference; drop it again. */
	if (card != NULL) {
		put_device(&card->dev);
	}
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	if (result == 0) {
		memcpy(app.partitions, partitions, count * sizeof(partitions[0]));
		app.partition_count = count;
	}
	if (card_info_valid) {
		app.card = card_info;
		app.card_info_valid = true;
	}
	mutex_unlock(&app_lock);
	return result;
}
