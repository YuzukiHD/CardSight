// SPDX-License-Identifier: GPL-2.0-or-later
/* Erasing and formatting one partition, with progress for the banner. */

#include "app/app_internal.h"


static const char *format_name(enum blk_partition_content content)
{
	return content == BLK_PARTITION_FAT32 ? "fat32" : content == BLK_PARTITION_EXFAT ? "exfat" : "ext";
}


/* Progress is a percentage of the current stage, or -1 while the stage has no measure. */
static void app_set_progress(int percent, const char *operation)
{
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	if (percent == app.progress && operation == NULL) {
		mutex_unlock(&app_lock);
		return;
	}
	app.progress = percent;
	if (operation != NULL) {
		strcpy(app.operation, operation);
	}
	mutex_unlock(&app_lock);
	/* The serial log is plain ASCII and has no place for an unknown percentage. */
	if (percent >= 0 && (operation != NULL || percent % 10 == 0)) {
		pr_info("progress: %d%%\n", percent);
	}
	app_post_refresh_runtime();
}


/*
 * ext4 writes its inode tables below the highest sector it has touched, so a percentage would sit
 * near the end for most of the run. It shows the elapsed time instead, once a second.
 */
static void format_progress(void *context, uint64_t done, uint64_t total)
{
	(void)context;
	app_set_progress(total == 0U ? 0 : (int)(done * 100U / total), NULL);
}


/* Full format: overwrite the whole partition with zeros before the file system is written. */
static int wipe_partition(const char *device)
{
	enum { CHUNK_SECTORS = 512 };
	struct block_device *bdev = bdev_open_by_name(device);
	uint64_t total;
	uint64_t sector = 0U;
	uint8_t *zeros_block;
	uint8_t *zeros;
	int result = 0;

	if (bdev == NULL) {
		return -ENODEV;
	}
	if (bdev_read_only(bdev)) {
		bdev_close(bdev);
		return -EROFS;
	}
	/* The C library heap is small; the 64 byte aligned chunk comes from the kernel heap. */
	zeros_block = pvPortMalloc(CHUNK_SECTORS * 512U + 63U);
	if (zeros_block == NULL) {
		bdev_close(bdev);
		return -ENOMEM;
	}
	zeros = (uint8_t *)(((uintptr_t)zeros_block + 63U) & ~(uintptr_t)63U);
	memset(zeros, 0, CHUNK_SECTORS * 512U);
	total = bdev_nr_sectors(bdev);
	app_set_progress(0, "正在清除");
	while (result == 0 && sector < total) {
		uint32_t count = total - sector < CHUNK_SECTORS ? (uint32_t)(total - sector) : CHUNK_SECTORS;

		result = bdev_write(bdev, sector, zeros, count);
		sector += count;
		app_set_progress((int)(sector * 100U / total), NULL);
	}
	if (result == 0) {
		result = bdev_flush(bdev);
	}
	vPortFree(zeros_block);
	bdev_close(bdev);
	return result;
}


int format_partition(uint32_t partition, enum blk_partition_content content, bool full)
{
	char device[VFS_BLK_NAME];
	char label[BDEV_LABEL_LEN];
	uint64_t started_ms;
	int result = 0;

	snprintf(device, sizeof(device), "%sp%u", CONFIG_STORAGE_PROXY_SOURCE_DISK, partition);
	snprintf(label, sizeof(label), "CARD%u", partition);
	if (full) {
		result = wipe_partition(device);
	}
	if (result == 0) {
		app_set_progress(0, "正在格式化");
		vfs_set_format_progress(format_progress, NULL);
		started_ms = ktime_get_ms();
		result = vfs_format(device, format_name(content), label, content == BLK_PARTITION_EXT4 ? 4U : 0U);
		vfs_set_format_progress(NULL, NULL);
		pr_info("format: %s on %s finished in %llu ms, result %d\n", format_name(content), device,
			(unsigned long long)(ktime_get_ms() - started_ms), result);
	}
	return result;
}
