/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Block devices: open by name, the checked transfers, and the mount claim a file system uses.
 */

#include <string.h>

#include "block/block_internal.h"

struct block_device *bdev_open_by_name(const char *name)
{
	struct block_device *bdev;

	if (name == NULL) {
		return NULL;
	}
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

void bdev_release_mount(struct block_device *bdev)
{
	blk_lock();
	bdev->mounts = 0U;
	blk_unlock();
}

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

int bdev_read(struct block_device *bdev, uint64_t sector, void *buffer, uint32_t count)
{
	int result = range_check(bdev, sector, count);

	if (result != 0) {
		return result;
	}
	return bdev->bd_disk->fops->read(bdev->bd_disk, bdev->start_sect + sector, buffer, count);
}

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

int bdev_flush(struct block_device *bdev)
{
	if (bdev == NULL || bdev->bd_disk->dead) {
		return -ENODEV;
	}
	return bdev->bd_disk->fops->flush != NULL ? bdev->bd_disk->fops->flush(bdev->bd_disk) : 0;
}

bool bdev_read_only(const struct block_device *bdev)
{
	return bdev->read_only || (bdev->bd_disk->flags & GENHD_FL_RDONLY) != 0U;
}

uint64_t bdev_nr_sectors(const struct block_device *bdev)
{
	return bdev->nr_sects;
}

uint32_t bdev_sector_size(const struct block_device *bdev)
{
	return bdev->bd_disk->logical_block_size;
}

bool bdev_is_whole(const struct block_device *bdev)
{
	return bdev->partno == 0U;
}

const char *bdev_name(const struct block_device *bdev)
{
	return bdev->dev.name;
}
