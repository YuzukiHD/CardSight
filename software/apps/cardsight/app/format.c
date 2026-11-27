// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file format.c
 * @brief Erasing and formatting one partition, with progress for the banner.
 */

#include "app/app_private.h"


/**
 * @brief Return the VFS file system name for a partition content type.
 *
 * @param[in] content File system to create.
 * @return "fat32", "exfat" or "ext".
 */
static const char *format_name(enum blk_partition_content content)
{
	return content == BLK_PARTITION_FAT32 ? "fat32" : content == BLK_PARTITION_EXFAT ? "exfat" : "ext";
}


/**
 * @brief Publish format progress and optionally a new operation text.
 *
 * @details Progress is a percentage of the current stage, or -1 while the
 * stage has no measure. Duplicate percentages are dropped only when the
 * operation text does not change, which keeps the refresh traffic low without
 * losing a stage transition. Only multiples of ten are logged, because the
 * serial log is plain ASCII and has no place for an unknown percentage.
 *
 * @param[in] percent   Progress percentage, or -1 when it is not measurable.
 * @param[in] operation New operation text, or NULL to keep the current one.
 */
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


/**
 * @brief Progress callback handed to the VFS formatter.
 *
 * @details ext4 writes its inode tables below the highest sector it has
 * touched, so a percentage would sit near the end for most of the run; the
 * callback still scales the sectors the formatter reports. A zero total is
 * reported as zero rather than dividing by it.
 *
 * @param[in] context Unused.
 * @param[in] done    Work units finished.
 * @param[in] total   Work units of the stage.
 */
static void format_progress(void *context, uint64_t done, uint64_t total)
{
	(void)context;
	app_set_progress(total == 0U ? 0 : (int)(done * 100U / total), NULL);
}


/**
 * @brief Overwrite the whole partition with zeros before the file system is written.
 *
 * @details Full format: the partition is erased in 512-sector chunks of zeros
 * and flushed once at the end, so the card sees one large sequential stream.
 * The chunk comes from the kernel heap because the C library heap is small; it
 * is allocated with 63 bytes of slack and rounded up so every write is 64-byte
 * aligned. Progress is the fraction of the partition already erased.
 *
 * @param[in] device Block device name of the partition.
 * @return 0 on success, or a negative errno.
 */
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
	app_set_progress(0, "Wiping");
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


/**
 * @brief Format one partition, optionally erasing it first.
 *
 * @details A full format wipes the partition before the file system is
 * written; a quick format only writes the metadata. The VFS progress callback
 * is installed for the duration of the format so the banner advances, and is
 * cleared again even when the format fails. The label is derived from the
 * partition number, and ext4 is asked for four reserved blocks.
 *
 * @param[in] partition Partition number, one-based.
 * @param[in] content   File system to create.
 * @param[in] full      True to zero the whole partition first.
 * @return 0 on success, or a negative errno.
 */
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
		app_set_progress(0, "Formatting");
		vfs_set_format_progress(format_progress, NULL);
		started_ms = ktime_get_ms();
		result = vfs_format(device, format_name(content), label, content == BLK_PARTITION_EXT4 ? 4U : 0U);
		vfs_set_format_progress(NULL, NULL);
		pr_info("format: %s on %s finished in %llu ms, result %d\n", format_name(content), device,
			(unsigned long long)(ktime_get_ms() - started_ms), result);
	}
	return result;
}
