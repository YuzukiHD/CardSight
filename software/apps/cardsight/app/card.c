// SPDX-License-Identifier: GPL-2.0-or-later
/* Reading the card: what is present, its table type, its partitions and its identity. */

#include "app/app_internal.h"


bool card_disk_present(void)
{
	struct block_device *disk = bdev_open_by_name(CONFIG_STORAGE_PROXY_SOURCE_DISK);

	if (disk == NULL) {
		return false;
	}
	bdev_close(disk);
	return true;
}


static enum blk_partition_table_type detect_table(struct block_device *bdev)
{
	uint8_t sector[512] __attribute__((aligned(64)));

	if (bdev_read(bdev, 0U, sector, 1U) == 0 && sector[510] == 0x55U && sector[511] == 0xaaU &&
		sector[450] == 0xeeU && bdev_read(bdev, 1U, sector, 1U) == 0 && memcmp(sector, "EFI PART", 8U) == 0) {
		return BLK_PARTITION_TABLE_GPT;
	}
	return BLK_PARTITION_TABLE_MBR;
}


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
