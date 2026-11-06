/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Disks: allocation, registration and removal, and the device reference the block devices use.
 *
 * The disk list is protected by one lock. A disk owns its partitions; a partition holds a
 * reference on its disk, so an open partition stays valid after the disk was taken away (its
 * transfers fail once the disk is dead).
 */

#define LOG_MODULE "block"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shell.h>

#include <bits.h>
#include <hal/delay.h>
#include <hal/mutex.h>
#include <log.h>
#include <size.h>

#include "block/block_internal.h"


static struct gendisk *disk_list;

#ifdef CARDSIGHT_HOST_TEST
void blk_lock(void) {}
void blk_unlock(void) {}
#else
static struct mutex block_lock;
static bool block_lock_ready;

void blk_lock(void)
{
	if (!block_lock_ready) {
		mutex_init(&block_lock);
		block_lock_ready = true;
	}
	mutex_lock_timeout(&block_lock, WAIT_FOREVER);
}

void blk_unlock(void)
{
	mutex_unlock(&block_lock);
}
#endif

/* The last reference of a block device is gone. */
void bdev_release(struct device *dev)
{
	struct block_device *bdev = container_of(dev, struct block_device, dev);
	struct gendisk *disk = bdev->bd_disk;

	if (bdev->partno == 0U) {
		if (disk->fops != NULL && disk->fops->free_disk != NULL) {
			disk->fops->free_disk(disk);
		}
		free(disk);
		free(bdev);
	} else {
		free(bdev);
		put_device(&disk->part0->dev);
	}
}

void device_initialize(struct device *dev)
{
	memset(dev, 0, sizeof(*dev));
	dev->refcount = 1;
}

void get_device(struct device *dev)
{
	if (dev != NULL) {
		dev->refcount++;
	}
}

struct gendisk *alloc_disk(void)
{
	struct gendisk *disk = calloc(1, sizeof(*disk));
	struct block_device *part0 = calloc(1, sizeof(*part0));

	if (disk == NULL || part0 == NULL) {
		free(disk);
		free(part0);
		return NULL;
	}
	device_initialize(&part0->dev);
	part0->dev.release = bdev_release;
	part0->bd_disk = disk;
	disk->part0 = part0;
	disk->logical_block_size = BLOCK_SECTOR_SIZE;
	return disk;
}

void put_disk(struct gendisk *disk)
{
	if (disk != NULL) {
		put_device(&disk->part0->dev);
	}
}

/* the lock is held */
static bool name_exists(const char *name)
{
	struct gendisk *disk;

	for (disk = disk_list; disk != NULL; disk = disk->next) {
		if (strcmp(disk->part0->dev.name, name) == 0) {
			return true;
		}
		for (struct block_device *bdev = disk->parts; bdev != NULL; bdev = bdev->next) {
			if (strcmp(bdev->dev.name, name) == 0) {
				return true;
			}
		}
	}
	return false;
}

/* the lock is held */
struct block_device *find_locked(const char *name)
{
	struct gendisk *disk;

	for (disk = disk_list; disk != NULL; disk = disk->next) {
		if (strcmp(disk->part0->dev.name, name) == 0) {
			return disk->part0;
		}
		for (struct block_device *bdev = disk->parts; bdev != NULL; bdev = bdev->next) {
			if (strcmp(bdev->dev.name, name) == 0) {
				return bdev;
			}
		}
	}
	return NULL;
}

int add_disk(struct gendisk *disk)
{
	int result;

	if (disk == NULL || disk->disk_name[0] == '\0' || disk->fops == NULL || disk->fops->read == NULL ||
	    ((disk->flags & GENHD_FL_RDONLY) == 0U && disk->fops->write == NULL) ||
	    disk->logical_block_size != BLOCK_SECTOR_SIZE) {
		return -EINVAL;
	}
	blk_lock();
	if (disk->registered) {
		blk_unlock();
		return -EINVAL;
	}
	if (name_exists(disk->disk_name)) {
		blk_unlock();
		return -EEXIST;
	}
	snprintf(disk->part0->dev.name, sizeof(disk->part0->dev.name), "%s", disk->disk_name);
	disk->part0->dev.refcount = 1; /* the caller's reference */
	get_device(&disk->part0->dev); /* the registry's reference */
	disk->part0->nr_sects = disk->capacity;
	disk->part0->start_sect = 0U;
	disk->part0->partno = 0U;
	disk->part0->read_only = (disk->flags & GENHD_FL_RDONLY) != 0U;
	disk->dead = false;
	disk->registered = true;
	disk->next = disk_list;
	disk_list = disk;

	result = blk_scan_partitions(disk);
	if (result == -ENOENT) {
		result = 0; /* a disk without a table is one block device */
	} else if (result != 0) {
		pr_warn("%s: reading the partition table failed: %d\n", disk->disk_name, result);
		result = 0;
	}
	blk_unlock();
	return result;
}

bool disk_has_mounts(const struct gendisk *disk)
{
	if (disk->part0->mounts != 0U) {
		return true;
	}
	for (const struct block_device *bdev = disk->parts; bdev != NULL; bdev = bdev->next) {
		if (bdev->mounts != 0U) {
			return true;
		}
	}
	return false;
}

void disk_remove(struct gendisk *disk)
{
	struct gendisk **link = &disk_list;

	disk->dead = true;
	blk_drop_partitions(disk);
	while (*link != NULL && *link != disk) {
		link = &(*link)->next;
	}
	if (*link == disk) {
		*link = disk->next;
	}
	disk->registered = false;
	put_device(&disk->part0->dev); /* the registry's reference */
}

int del_gendisk(struct gendisk *disk)
{
	int result = 0;

	blk_lock();
	if (!disk->registered) {
		result = -ENODEV;
	} else if (disk_has_mounts(disk)) {
		result = -EBUSY;
	} else {
		disk_remove(disk);
	}
	blk_unlock();
	return result;
}

void del_gendisk_force(struct gendisk *disk)
{
	blk_lock();
	if (disk->registered) {
		disk_remove(disk);
	}
	blk_unlock();
}

#ifdef CONFIG_SHELL
static int command_blk(int argc, char **argv)
{
	struct gendisk *disk;

	(void)argc;
	(void)argv;
	blk_lock();
	for (disk = disk_list; disk != NULL; disk = disk->next) {
		char capacity[16];

		format_capacity(disk->capacity * 512U, capacity, sizeof(capacity));
		printf("%-8s %10llu sectors (%s)%s\n", disk->disk_name, (unsigned long long)disk->capacity,
			capacity, disk->part0->mounts != 0U ? ", mounted" : "");
		for (struct block_device *bdev = disk->parts; bdev != NULL; bdev = bdev->next) {
			format_capacity(bdev->nr_sects * 512U, capacity, sizeof(capacity));
			printf("%-8s %10llu sectors (%s), at %llu, type %02x%s\n", bdev->dev.name,
				(unsigned long long)bdev->nr_sects, capacity,
				(unsigned long long)bdev->start_sect, bdev->part_type,
				bdev->mounts != 0U ? ", mounted" : "");
		}
	}
	blk_unlock();
	return 0;
}
SHELL_CMD(blk, "list the block devices", command_blk);

static int command_blkread(int argc, char **argv)
{
	uint8_t sector[512] __attribute__((aligned(64)));
	struct block_device *bdev;
	uint64_t number;
	int result;

	if (argc < 3) {
		return -EINVAL;
	}
	bdev = bdev_open_by_name(argv[1]);
	if (bdev == NULL) {
		return -ENODEV;
	}
	number = strtoul(argv[2], NULL, 0);
	result = bdev_read(bdev, number, sector, 1);
	if (result == 0) {
		printf("sector %llu:", (unsigned long long)number);
		for (unsigned int index = 0U; index < 64U; index++) {
			printf(" %02x", sector[index]);
		}
		printf("\n");
	} else {
		printf("read failed: %d\n", result);
	}
	bdev_close(bdev);
	return result;
}
SHELL_CMD(blkread, "blkread <dev> <sector>", command_blkread);

#ifndef CARDSIGHT_HOST_TEST
/* Read @p mib MiB from the disk in 256 KiB requests and report the throughput. */
static int command_blkbench(int argc, char **argv)
{
	struct block_device *bdev;
	const uint32_t chunk_sectors = 512U;
	uint64_t sector;
	unsigned int mib;
	uint64_t total_sectors;
	uint64_t done_sectors = 0U;
	void *buffer;
	uint64_t started;
	uint64_t elapsed;
	int result = 0;

	if (argc < 4) {
		return -EINVAL;
	}
	bdev = bdev_open_by_name(argv[1]);
	if (bdev == NULL) {
		return -ENODEV;
	}
	sector = strtoul(argv[2], NULL, 0);
	mib = (unsigned int)strtoul(argv[3], NULL, 0);
	total_sectors = (uint64_t)mib * 2048U;
	buffer = aligned_alloc(64U, chunk_sectors * 512U);
	if (buffer == NULL) {
		bdev_close(bdev);
		return -ENOMEM;
	}
	started = ktime_get_ms();
	while (done_sectors < total_sectors && result == 0) {
		uint32_t blocks = (uint32_t)MIN((uint64_t)chunk_sectors, total_sectors - done_sectors);

		result = bdev_read(bdev, sector + done_sectors, buffer, blocks);
		if (result == 0) {
			done_sectors += blocks;
			if (done_sectors % (16U * 2048U) == 0U || done_sectors >= total_sectors) {
				printf("  %u MiB\n", (unsigned)(done_sectors / 2048U));
			}
		}
	}
	elapsed = ktime_get_ms() - started;
	if (result != 0) {
		printf("read failed at %u MiB: %d\n", (unsigned)(done_sectors / 2048U), result);
	} else {
		uint64_t kib = done_sectors / 2U; /* 512 byte sectors to KiB */

		printf("%u MiB in %llu ms, %llu KiB/s\n", (unsigned)(done_sectors / 2048U),
			(unsigned long long)elapsed,
			(unsigned long long)(elapsed != 0U ? kib * 1000U / elapsed : 0U));
	}
	free(buffer);
	bdev_close(bdev);
	return result;
}
SHELL_CMD(blkbench, "blkbench <dev> <sector> <MiB>", command_blkbench);
#endif /* CARDSIGHT_HOST_TEST */
#endif
