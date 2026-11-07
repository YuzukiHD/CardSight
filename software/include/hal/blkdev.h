/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file blkdev.h
 * @brief Block devices: disks made by a driver, the partitions found in their table, and the
 * checked transfers on them.
 *
 * A disk driver fills a struct gendisk from alloc_disk() and registers it with add_disk(); the
 * whole disk is "part0" and the partitions are "<disk>p<N>". Access goes through a struct
 * block_device opened by name.
 *
 * The calls are synchronous and run in the caller's task; read and write buffers must be
 * 64-byte aligned (the card driver does DMA on them).
 */

#ifndef CARDSIGHT_BLKDEV_H
#define CARDSIGHT_BLKDEV_H

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <hal/device.h>

/** @brief Longest disk name, including the terminating zero. */
#define DISK_NAME_LEN  16
/** @brief Longest partition label in bytes. */
#define BDEV_LABEL_LEN 32

/** @brief Disk flag: write and discard are refused. */
#define GENHD_FL_RDONLY 0x02

/** @brief A registered disk (defined below). */
struct gendisk;

/**
 * @brief Operations a disk driver provides to the block layer.
 */
struct block_device_operations {
	/**
	 * @brief Read sectors.
	 *
	 * Sector numbers are relative to the disk, the range was checked, and the buffer is
	 * 64-byte aligned.
	 */
	int (*read)(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count);
	/** @brief Write sectors; may be NULL on a disk with GENHD_FL_RDONLY. */
	int (*write)(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count);
	/** @brief Write out what the disk holds back; NULL when it writes through. */
	int (*flush)(struct gendisk *disk);
	/**
	 * @brief Release driver state when the last reference is gone.
	 *
	 * The disk was deleted and nobody has it open; free @c private_data here.
	 */
	void (*free_disk)(struct gendisk *disk);
};

/**
 * @brief One openable view of a disk: the whole disk or one partition.
 */
struct block_device {
	/** @brief Backing device; the name is the disk name or "<disk>p<N>". */
	struct device dev;
	struct gendisk *bd_disk; /**< Disk this view belongs to. */
	uint32_t partno; /**< Partition number; 0 is the whole disk. */
	uint64_t start_sect; /**< First sector in the disk. */
	uint64_t nr_sects; /**< Length of the view in sectors. */
	uint8_t part_type; /**< Partition table type byte; 0 for a disk or without a table. */
	bool read_only; /**< Whether this view refuses writes. */
	char label[BDEV_LABEL_LEN]; /**< Partition label, empty when none. */
	uint32_t openers; /**< bdev_open_by_name() calls without bdev_close() yet. */
	uint32_t mounts;  /**< File systems mounted from it. */
	struct block_device *next; /**< Next partition of the disk. */
};

/**
 * @brief A disk as a driver describes it to the block layer.
 */
struct gendisk {
	char disk_name[DISK_NAME_LEN]; /**< Set by the driver before add_disk(). */
	void *private_data; /**< Driver-private state. */
	const struct block_device_operations *fops; /**< Driver operations. */
	uint64_t capacity; /**< Sectors of logical_block_size bytes. */
	uint32_t logical_block_size; /**< Bytes per sector; 512. */
	uint32_t flags; /**< GENHD_FL_* flags. */
	struct block_device *part0; /**< The whole disk. */
	struct block_device *parts; /**< The partitions, in partition number order. */
	uint32_t part_count; /**< Number of partitions in @c parts. */
	struct gendisk *next; /**< The registered disks. */
	bool registered; /**< Whether add_disk() succeeded. */
	bool dead; /**< del_gendisk() ran: every access fails with -ENODEV. */
};

/**
 * @brief Allocate a zeroed disk with its whole-disk block device.
 *
 * @return The disk, with @c part0 ready, or NULL when out of memory.
 */
struct gendisk *alloc_disk(void);

/**
 * @brief Drop the caller's reference to a disk.
 *
 * Call after del_gendisk(), or after a failed add_disk().
 *
 * @param[in] disk Disk to release; NULL is ignored.
 */
void put_disk(struct gendisk *disk);

/**
 * @brief Register the disk and read its partition table.
 *
 * @details The name and operations are validated, then the whole-disk view is registered and
 * the MBR or GPT is scanned. A table that cannot be read is logged and the disk stays without
 * partitions.
 *
 * @param[in] disk Disk to register.
 * @return 0 on success, -EINVAL for a bad description, or -EEXIST when the name is taken.
 */
int add_disk(struct gendisk *disk);

/**
 * @brief Take the disk and its partitions away.
 *
 * @param[in] disk Disk to remove.
 * @return 0 on success, -ENODEV when it is not registered, or -EBUSY while one of its block
 * devices is mounted.
 */
int del_gendisk(struct gendisk *disk);

/**
 * @brief Remove the disk without checking its mounts.
 *
 * The same as del_gendisk() but without the -EBUSY, for a card that is gone.
 *
 * @param[in] disk Disk to remove.
 */
void del_gendisk_force(struct gendisk *disk);

/**
 * @brief Open a registered block device by name.
 *
 * @param[in] name Device name such as "mmcblk0", "mmcblk0p1", or "/dev/...".
 * @return The block device with a reference, or NULL when no live device matches.
 */
struct block_device *bdev_open_by_name(const char *name);
/**
 * @brief Drop one opener of a block device.
 *
 * @details The last close of a live disk flushes it, so writes it held back reach the media.
 *
 * @param[in] bdev Device to close; NULL is ignored.
 */
void bdev_close(struct block_device *bdev);

/**
 * @brief Read sectors through a block device view.
 *
 * @param[in]  bdev   Device to read from.
 * @param[in]  sector First sector inside the device.
 * @param[out] buffer Destination buffer; 64-byte aligned.
 * @param[in]  count  Number of sectors.
 * @return 0 on success, or a negative errno when the device is gone or the range is invalid.
 */
int bdev_read(struct block_device *bdev, uint64_t sector, void *buffer, uint32_t count);
/**
 * @brief Write sectors through a block device view.
 *
 * @param[in] bdev   Device to write to.
 * @param[in] sector First sector inside the device.
 * @param[in] buffer Source buffer; 64-byte aligned.
 * @param[in] count  Number of sectors.
 * @return 0 on success, -EROFS for a read-only device, or another negative errno.
 */
int bdev_write(struct block_device *bdev, uint64_t sector, const void *buffer, uint32_t count);
/**
 * @brief Ask the disk behind a block device to write out its cached writes.
 *
 * @param[in] bdev Device to flush.
 * @return 0 on success, or -ENODEV when the device or its disk is gone.
 */
int bdev_flush(struct block_device *bdev);

/**
 * @brief Report whether a block device refuses writes.
 *
 * @param[in] bdev Device to query.
 * @return true when the view or the whole disk is read-only.
 */
bool bdev_read_only(const struct block_device *bdev);
/**
 * @brief Length of a block device in sectors.
 *
 * @param[in] bdev Device to query.
 * @return The number of sectors in the view.
 */
uint64_t bdev_nr_sectors(const struct block_device *bdev);
/**
 * @brief Logical sector size of the disk behind a block device.
 *
 * @param[in] bdev Device to query.
 * @return The block size in bytes.
 */
uint32_t bdev_sector_size(const struct block_device *bdev);
/**
 * @brief Report whether a block device covers a whole disk.
 *
 * @param[in] bdev Device to query.
 * @return true for the disk's partition 0, false for a partition.
 */
bool bdev_is_whole(const struct block_device *bdev);
/**
 * @brief Name of a block device as registered.
 *
 * @param[in] bdev Device to query.
 * @return The device name, without the "/dev/" prefix.
 */
const char *bdev_name(const struct block_device *bdev);

/**
 * @brief Claim a block device for one file system instance.
 *
 * A file system keeps a device claimed from its mount to its unmount.
 *
 * @param[in] bdev Device to claim.
 * @return 0 on success, -ENODEV when the disk is gone, or -EBUSY when already mounted.
 */
int bdev_claim_mount(struct block_device *bdev);
/** @brief Release the mount claim of a block device. */
void bdev_release_mount(struct block_device *bdev);

/* ---- partition tables ---- */

/**
 * @brief Partition table format written by blk_write_partition_layout().
 */
enum blk_partition_table_type {
	BLK_PARTITION_TABLE_MBR, /**< Master boot record, four primary entries. */
	BLK_PARTITION_TABLE_GPT, /**< GUID partition table, primary and backup copies. */
};

/**
 * @brief File system marker stored in a created partition.
 */
enum blk_partition_content {
	BLK_PARTITION_FAT32, /**< FAT32 partition type. */
	BLK_PARTITION_EXFAT, /**< exFAT partition type. */
	BLK_PARTITION_EXT4, /**< Linux ext4 partition type. */
};

/**
 * @brief One partition to create, in disk order.
 */
struct blk_partition_layout_entry {
	uint64_t sectors; /**< Size in sectors; 0 means the rest of the disk. */
	enum blk_partition_content content; /**< File system marker for the partition. */
	char label[BDEV_LABEL_LEN]; /**< Partition label. */
};

/**
 * @brief Erase the MBR and both GPT copies.
 *
 * @details Zeroes the first 512 KiB (some cards carry a boot image in front of the table and
 * the SoC would try to run it) and the backup GPT copy at the end of the disk, then rescans,
 * leaving the card without a partition table. File data itself is left where it is.
 *
 * @param[in] disk Registered disk to erase.
 * @return 0 on success, -ENODEV for an unregistered disk, -ENOTSUP for a non-512-byte sector
 * size, or -EBUSY while a partition is mounted.
 */
int blk_erase_partition_table(struct gendisk *disk);

/**
 * @brief Write a fresh partition table and rescan it.
 *
 * @details Entries are placed with 1 MiB alignment, at most four of them for the MBR; the first
 * entry starts at sector 2048. The table is flushed and rescanned before the call returns.
 *
 * @param[in] disk    Registered disk to write.
 * @param[in] table   Table format to write.
 * @param[in] entries Partitions to create, in disk order.
 * @param[in] count   Number of entries in @p entries.
 * @return 0 on success, -ENODEV for an unregistered disk, -ENOTSUP for a non-512-byte sector
 * size, -EINVAL for an unknown table, -ENOSPC when the sizes do not fit, or -EBUSY while a
 * partition is mounted.
 */
int blk_write_partition_layout(struct gendisk *disk, enum blk_partition_table_type table,
	const struct blk_partition_layout_entry *entries, uint32_t count);

#endif /* CARDSIGHT_BLKDEV_H */
