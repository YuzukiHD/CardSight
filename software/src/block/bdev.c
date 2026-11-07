/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file bdev.c
 * @brief Block devices: open by name, the checked transfers, and the mount claim.
 *
 * A block device is a view of a disk or of one partition on it. Opening takes a reference and
 * records the opener; every transfer is range checked against the view before it reaches the
 * disk's operations.
 */

#include <string.h>

#include "block/block_internal.h"

/**
 * @brief Open a block device by name and take a reference on it.
 *
 * @details Accepts the name with or without the "/dev/" prefix. The lookup runs under the
 * block lock and refuses a device whose disk is already dead; on success the opener count is
 * raised and the device reference taken, so the disk cannot disappear underneath the caller.
 *
 * @param[in] name Device name, e.g. "mmcblk0p1" or "/dev/mmcblk0".
 * @return The block device, or NULL when @p name is NULL or no live device matches.
 */
struct block_device *bdev_open_by_name(const char *name)
{
	struct block_device *bdev;

	if (name == NULL) {
		return NULL;
	}
	/* the shell and file systems both spell names with the prefix */
	if (strncmp(name, "/dev/", 5) == 0) {
		name += 5;
	}
	blk_lock();
	bdev = find_locked(name);
	if (bdev != NULL && bdev->bd_disk->dead) {
		bdev = NULL;
	}
	if (bdev != NULL) {
		bdev->openers++;
		get_device(&bdev->dev);
	}
	blk_unlock();
	return bdev;
}

/**
 * @brief Drop one opener of a block device.
 *
 * @details The count is decremented under the block lock. When the last opener of a live disk
 * closes, the disk is flushed once, so writes it held back reach the media; a dead disk is not
 * touched. The reference is released in every case, even when the device is already dead.
 *
 * @param[in] bdev Device to close; NULL is ignored.
 */
void bdev_close(struct block_device *bdev)
{
	bool last;

	if (bdev == NULL) {
		return;
	}
	blk_lock();
	if (bdev->openers != 0U) {
		bdev->openers--;
	}
	last = bdev->openers == 0U && !bdev->bd_disk->dead;
	blk_unlock();
	/* the last close lets a disk that holds writes back write them out */
	if (last) {
		(void)bdev_flush(bdev);
	}
	put_device(&bdev->dev);
}

/**
 * @brief Claim a block device for one file system instance.
 *
 * @details Refuses a dead disk and a device another instance already mounted; otherwise the
 * mount flag is set under the block lock.
 *
 * @param[in] bdev Device to claim.
 * @return 0 on success, -ENODEV when the disk is gone, or -EBUSY when already mounted.
 */
int bdev_claim_mount(struct block_device *bdev)
{
	int result = 0;

	blk_lock();
	if (bdev->bd_disk->dead) {
		result = -ENODEV;
	} else if (bdev->mounts != 0U) {
		result = -EBUSY;
	} else {
		bdev->mounts = 1U;
	}
	blk_unlock();
	return result;
}

/**
 * @brief Release the mount claim of a block device.
 *
 * @param[in] bdev Device to release.
 */
void bdev_release_mount(struct block_device *bdev)
{
	blk_lock();
	bdev->mounts = 0U;
	blk_unlock();
}

/**
 * @brief Check that a transfer stays inside a block device.
 *
 * @details Rejects a missing device and a dead disk first, then compares the sector range
 * against the view's own length, so a partition cannot read past its end. The subtraction
 * form avoids overflow when @p count is huge.
 *
 * @param[in] bdev   Device the transfer targets.
 * @param[in] sector First sector inside the device.
 * @param[in] count  Number of sectors.
 * @return 0 when the range is valid, -ENODEV or -EINVAL otherwise.
 */
static int range_check(const struct block_device *bdev, uint64_t sector, uint32_t count)
{
	if (bdev == NULL) {
		return -ENODEV;
	}
	if (bdev->bd_disk->dead) {
		return -ENODEV;
	}
	if (sector > bdev->nr_sects || count > bdev->nr_sects - sector) {
		return -EINVAL;
	}
	return 0;
}

/**
 * @brief Read sectors through a block device view.
 *
 * @details The range is checked against the view, then the request is shifted by the view's
 * start sector and handed to the disk's read operation.
 *
 * @param[in]  bdev   Device to read from.
 * @param[in]  sector First sector inside the device.
 * @param[out] buffer Destination buffer.
 * @param[in]  count  Number of sectors.
 * @return 0 on success, or a negative errno.
 */
int bdev_read(struct block_device *bdev, uint64_t sector, void *buffer, uint32_t count)
{
	int result = range_check(bdev, sector, count);

	if (result != 0) {
		return result;
	}
	return bdev->bd_disk->fops->read(bdev->bd_disk, bdev->start_sect + sector, buffer, count);
}

/**
 * @brief Write sectors through a block device view.
 *
 * @details Checks the range and the read-only state of the view or of the disk, then shifts
 * the request by the view's start sector and hands it to the disk's write operation.
 *
 * @param[in] bdev   Device to write to.
 * @param[in] sector First sector inside the device.
 * @param[in] buffer Source buffer.
 * @param[in] count  Number of sectors.
 * @return 0 on success, -EROFS for a read-only view, or another negative errno.
 */
int bdev_write(struct block_device *bdev, uint64_t sector, const void *buffer, uint32_t count)
{
	int result = range_check(bdev, sector, count);

	if (result != 0) {
		return result;
	}
	if (bdev_read_only(bdev)) {
		return -EROFS;
	}
	return bdev->bd_disk->fops->write(bdev->bd_disk, bdev->start_sect + sector, buffer, count);
}

/**
 * @brief Ask the disk behind a block device to write out its cached writes.
 *
 * @details A disk without a flush operation has nothing to hold back, so the call succeeds.
 *
 * @param[in] bdev Device to flush.
 * @return 0 on success, or -ENODEV when the device or its disk is gone.
 */
int bdev_flush(struct block_device *bdev)
{
	if (bdev == NULL || bdev->bd_disk->dead) {
		return -ENODEV;
	}
	return bdev->bd_disk->fops->flush != NULL ? bdev->bd_disk->fops->flush(bdev->bd_disk) : 0;
}

/**
 * @brief Report whether a block device refuses writes.
 *
 * @return true when the view or the whole disk is read-only.
 */
bool bdev_read_only(const struct block_device *bdev)
{
	return bdev->read_only || (bdev->bd_disk->flags & GENHD_FL_RDONLY) != 0U;
}

/**
 * @brief Size of a block device in sectors.
 *
 * @return The number of 512-byte sectors.
 */
uint64_t bdev_nr_sectors(const struct block_device *bdev)
{
	return bdev->nr_sects;
}

/**
 * @brief Logical sector size of the disk behind a block device.
 *
 * @return The block size in bytes.
 */
uint32_t bdev_sector_size(const struct block_device *bdev)
{
	return bdev->bd_disk->logical_block_size;
}

/**
 * @brief Report whether a block device covers a whole disk.
 *
 * @return true for the disk's partition 0, false for a partition.
 */
bool bdev_is_whole(const struct block_device *bdev)
{
	return bdev->partno == 0U;
}

/**
 * @brief Name of a block device as registered.
 *
 * @return The device name without the "/dev/" prefix.
 */
const char *bdev_name(const struct block_device *bdev)
{
	return bdev->dev.name;
}
