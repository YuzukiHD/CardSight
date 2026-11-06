/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The block layer's own declarations: the lock that protects the disk list and the partition
 * helpers shared by add_disk() and the layout writer.
 */

#ifndef CARDSIGHT_BLOCK_INTERNAL_H
#define CARDSIGHT_BLOCK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <hal/blkdev.h>

#define BLOCK_SECTOR_SIZE 512U

void blk_lock(void);
void blk_unlock(void);

void bdev_release(struct device *dev);

/** A block device by name; the block lock is held. */
struct block_device *find_locked(const char *name);

void disk_remove(struct gendisk *disk);
bool disk_has_mounts(const struct gendisk *disk);

/** Read the table of @p disk and register its partitions; -ENOENT without a table. */
int blk_scan_partitions(struct gendisk *disk);
void blk_drop_partitions(struct gendisk *disk);
int blkdev_rescan_partitions(struct gendisk *disk);

static inline uint16_t blk_le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static inline uint32_t blk_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
		(uint32_t)bytes[3] << 24;
}

static inline uint64_t blk_le64(const uint8_t *bytes)
{
	return (uint64_t)blk_le32(bytes) | (uint64_t)blk_le32(bytes + 4) << 32;
}

static inline void blk_put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static inline void blk_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static inline void blk_put_le64(uint8_t *bytes, uint64_t value)
{
	blk_put_le32(bytes, (uint32_t)value);
	blk_put_le32(bytes + 4, (uint32_t)(value >> 32));
}

#endif /* CARDSIGHT_BLOCK_INTERNAL_H */
