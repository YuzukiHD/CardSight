/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file genhd.c
 * @brief Disks: allocation, registration and removal, and the device reference model.
 *
 * The disk list is protected by one lock. A disk owns its partitions; a partition holds a
 * reference on its disk, so an open partition stays valid after the disk was taken away (its
 * transfers fail once the disk is dead).
 */

/** @brief Log module name used by the pr_*() helpers. */
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


/** @brief Head of the registered disks; protected by the block lock. */
static struct gendisk *disk_list;

#ifdef CARDSIGHT_HOST_TEST
/** @brief Host test build: no locking, the test owns the state. */
void blk_lock(void) {}
/** @brief Host test build: no locking, the test owns the state. */
void blk_unlock(void) {}
#else
/** @brief Lock protecting @c disk_list and the block devices. */
static struct mutex block_lock;
/** @brief Whether @c block_lock was initialised; disks can appear before the scheduler runs. */
static bool block_lock_ready;

/**
 * @brief Take the block lock, initialising it on first use.
 *
 * @details The initialisation is lazy because disks register from early init, which runs
 * before there is a scheduler; the mutex primitives are usable at that point.
 */
void blk_lock(void)
{
	if (!block_lock_ready) {
		mutex_init(&block_lock);
		block_lock_ready = true;
	}
	mutex_lock_timeout(&block_lock, WAIT_FOREVER);
}

/**
 * @brief Release the block lock.
 */
void blk_unlock(void)
{
	mutex_unlock(&block_lock);
}
#endif

/**
 * @brief Drop the last reference of a block device.
 *
 * @details Called through @c dev->release. For partition 0 the free_disk callback of the disk
 * runs first, then the disk and the partition 0 record are freed. For a partition only the
 * record is freed, and the reference the partition held on its disk is dropped, which may
 * release the disk in turn.
 *
 * @param[in] dev Device whose reference count reached zero.
 */
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

/**
 * @brief Initialise a device record with one reference.
 *
 * @param[out] dev Device to initialise.
 */
void device_initialize(struct device *dev)
{
	memset(dev, 0, sizeof(*dev));
	dev->refcount = 1;
}

/**
 * @brief Take one more reference on a device.
 *
 * @param[in,out] dev Device to reference; NULL is ignored.
 */
void get_device(struct device *dev)
{
	if (dev != NULL) {
		dev->refcount++;
	}
}

/**
 * @brief Allocate an unregistered disk and its partition 0 record.
 *
 * @details Both records come from the heap, so nothing is released when the registry refuses
 * the disk later; put_disk() drops the single reference. The sector size defaults to 512
 * bytes, which is the only size the layer accepts.
 *
 * @return The disk, or NULL when either allocation fails.
 */
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

/**
 * @brief Drop the caller's reference on a disk.
 *
 * @details Releasing partition 0 releases the disk itself through bdev_release(), once the
 * registry's reference is gone too.
 *
 * @param[in] disk Disk to release; NULL is ignored.
 */
void put_disk(struct gendisk *disk)
{
	if (disk != NULL) {
		put_device(&disk->part0->dev);
	}
}

/**
 * @brief Report whether a device name is already taken.
 *
 * @details Checks every registered disk's own name and then each of its partitions, because a
 * name only has to be unique across the whole layer.
 *
 * @param[in] name Name to look for.
 * @return true when a disk or partition already uses @p name.
 * @note The block lock must be held.
 */
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

/**
 * @brief Look up a block device by name.
 *
 * @details A block device by name; the block lock is held. Each disk's partition 0 is matched
 * before its partitions.
 *
 * @param[in] name Name to look for, without the "/dev/" prefix.
 * @return The matching block device, or NULL.
 */
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

/**
 * @brief Publish a disk and scan its partition table.
 *
 * @details Validates the operations and the name, takes the block lock and refuses a disk that
 * is already registered or whose name is taken. Partition 0 is then named and sized, and gets
 * two references: the caller's and the registry's, the second dropped by disk_remove(). The
 * disk is linked at the head of the list and its table scanned while the lock is held, so the
 * partitions are visible before any name lookup can find the disk. A missing table is not an
 * error (a blank disk is one block device), and neither is a table that cannot be parsed: the
 * disk is published without partitions and the failure is only logged.
 *
 * @param[in] disk Disk to publish.
 * @return 0 on success, -EINVAL for incomplete operations or a second registration, or -EEXIST
 *         when the name is taken.
 */
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

	/* the disk is visible; its partitions join it under the same lock */
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

/**
 * @brief Report whether any block device of the disk is mounted.
 *
 * @details Checks partition 0 and then every partition.
 *
 * @param[in] disk Disk to inspect.
 * @return true when a mount claim is held.
 */
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

/**
 * @brief Unlink a dead disk from the list and free the registry's reference.
 *
 * @details Marks the disk dead first, so openers still holding it fail their transfers, then
 * releases its partitions, unlinks it from the list and drops the registry's reference.
 *
 * @param[in] disk Disk to remove.
 * @note The block lock must be held.
 */
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

/**
 * @brief Unregister a disk unless something still uses it.
 *
 * @details Refuses an unregistered disk and one with a mounted partition; the caller can then
 * close the mounts and retry. Removal itself happens under the block lock.
 *
 * @param[in] disk Disk to unregister.
 * @return 0 on success, -ENODEV when not registered, or -EBUSY when a partition is mounted.
 */
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

/**
 * @brief Unregister a disk even while it is mounted.
 *
 * @details Used when the media is gone: the mounts cannot be dropped by the block layer, and
 * their transfers fail through the dead flag instead.
 *
 * @param[in] disk Disk to unregister.
 */
void del_gendisk_force(struct gendisk *disk)
{
	blk_lock();
	if (disk->registered) {
		disk_remove(disk);
	}
	blk_unlock();
}

#ifdef CONFIG_SHELL
/**
 * @brief Shell command: list the disks and partitions with their sizes and mount state.
 *
 * @details Walks the disk list under the block lock, printing a line per disk and one per
 * partition with its offset and type byte.
 *
 * @param[in] argc Argument count, unused.
 * @param[in] argv Argument vector, unused.
 * @return Always 0.
 */
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

/**
 * @brief Shell command: hex dump the first 64 bytes of one sector.
 *
 * @details Opens the named device, reads one sector and prints its leading bytes; the open and
 * close balance the device reference.
 *
 * @param[in] argc Argument count, at least 3: the command, the device and the sector.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL for too few arguments, -ENODEV without the device, or the read
 *         error.
 */
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
		for (uint32_t index = 0U; index < 64U; index++) {
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
/**
 * @brief Shell command: read a number of MiB from a disk and report the throughput.
 *
 * @details Reads @p mib MiB in 256 KiB requests (512 sectors, a cache-line-aligned buffer) from
 * the given sector, printing progress every 16 MiB, then divides the sectors read by the
 * elapsed milliseconds. The buffer is allocated aligned so the DMA engine can use it directly.
 *
 * @param[in] argc Argument count, at least 4: the command, the device, the first sector and the
 *                 size in MiB.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL for too few arguments, -ENODEV without the device, -ENOMEM
 *         without the buffer, or the read error that stopped the loop.
 */
static int command_blkbench(int argc, char **argv)
{
	struct block_device *bdev;
	const uint32_t chunk_sectors = 512U;
	uint64_t sector;
	uint32_t mib;
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
	mib = (uint32_t)strtoul(argv[3], NULL, 0);
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
				printf("  %u MiB\n", (uint32_t)(done_sectors / 2048U));
			}
		}
	}
	elapsed = ktime_get_ms() - started;
	if (result != 0) {
		printf("read failed at %u MiB: %d\n", (uint32_t)(done_sectors / 2048U), result);
	} else {
		uint64_t kib = done_sectors / 2U; /* 512 byte sectors to KiB */

		printf("%u MiB in %llu ms, %llu KiB/s\n", (uint32_t)(done_sectors / 2048U),
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
