/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file block_internal.h
 * @brief The block layer's own declarations.
 *
 * The lock that protects the disk list and the partition helpers shared by add_disk() and the
 * layout writer.
 */

#ifndef CARDSIGHT_BLOCK_INTERNAL_H
#define CARDSIGHT_BLOCK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <hal/blkdev.h>

/** @brief Size of one block sector in bytes; the layer accepts nothing else. */
#define BLOCK_SECTOR_SIZE 512U

/**
 * @brief Take the lock that protects the disk list and the block devices.
 *
 * @details The lock is initialised lazily on first use, because disks can be registered from
 * early init before the scheduler runs.
 */
void blk_lock(void);

/** @brief Release the lock taken by blk_lock(). */
void blk_unlock(void);

/**
 * @brief Drop the last reference of a block device.
 *
 * @details Called through @c dev->release. A whole disk frees its @c gendisk and the partition
 * 0 record; a partition frees only itself and drops its reference on the disk.
 *
 * @param[in] dev Device whose reference count reached zero.
 */
void bdev_release(struct device *dev);

/**
 * @brief Look up a block device by name.
 *
 * @details A block device by name; the block lock is held.
 *
 * @param[in] name Name without the "/dev/" prefix.
 * @return The matching block device, or NULL.
 */
struct block_device *find_locked(const char *name);

/**
 * @brief Unlink a dead disk from the list and free the registry's reference.
 *
 * @details The block lock is held and the disk is already marked dead.
 *
 * @param[in] disk Disk to remove.
 */
void disk_remove(struct gendisk *disk);

/**
 * @brief Report whether any block device of the disk is mounted.
 *
 * @param[in] disk Disk to inspect.
 * @return true when partition 0 or any partition holds a mount claim.
 */
bool disk_has_mounts(const struct gendisk *disk);

/**
 * @brief Read the table of @p disk and register its partitions.
 *
 * @details Reads sector 0, rejects it when the 0x55aa signature is missing, and dispatches to
 * the MBR scanner or, for a protective MBR, to the GPT scanner. The block lock must be held.
 *
 * @param[in] disk Disk to scan.
 * @return 0 on success, -ENOENT without a table, or another negative errno on a read error.
 */
int blk_scan_partitions(struct gendisk *disk);

/**
 * @brief Release every partition of a disk.
 *
 * @details The block lock is held; each partition device drops its reference.
 *
 * @param[in] disk Disk whose partitions go away.
 */
void blk_drop_partitions(struct gendisk *disk);

/**
 * @brief Drop and rescan the partitions of a registered disk.
 *
 * @details Takes the block lock itself, refuses when a partition is mounted and turns the
 * "no table" result into success.
 *
 * @param[in] disk Disk to rescan.
 * @return 0 on success, -ENODEV, -ENOTSUP or -EBUSY on refusal, or a read error.
 */
int blkdev_rescan_partitions(struct gendisk *disk);

/**
 * @brief Read a little-endian 16-bit value.
 *
 * @param[in] bytes Two bytes.
 * @return The decoded value.
 */
static inline uint16_t blk_le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

/**
 * @brief Read a little-endian 32-bit value.
 *
 * @param[in] bytes Four bytes.
 * @return The decoded value.
 */
static inline uint32_t blk_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
		(uint32_t)bytes[3] << 24;
}

/**
 * @brief Read a little-endian 64-bit value.
 *
 * @param[in] bytes Eight bytes.
 * @return The decoded value.
 */
static inline uint64_t blk_le64(const uint8_t *bytes)
{
	return (uint64_t)blk_le32(bytes) | (uint64_t)blk_le32(bytes + 4) << 32;
}

/**
 * @brief Store a 16-bit value in little-endian order.
 *
 * @param[out] bytes Two bytes to fill.
 * @param[in]  value Value to encode.
 */
static inline void blk_put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/**
 * @brief Store a 32-bit value in little-endian order.
 *
 * @param[out] bytes Four bytes to fill.
 * @param[in]  value Value to encode.
 */
static inline void blk_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/**
 * @brief Store a 64-bit value in little-endian order.
 *
 * @param[out] bytes Eight bytes to fill.
 * @param[in]  value Value to encode.
 */
static inline void blk_put_le64(uint8_t *bytes, uint64_t value)
{
	blk_put_le32(bytes, (uint32_t)value);
	blk_put_le32(bytes + 4, (uint32_t)(value >> 32));
}

#endif /* CARDSIGHT_BLOCK_INTERNAL_H */
