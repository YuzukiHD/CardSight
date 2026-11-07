/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Block devices: disks made by a driver, the partitions found in their table, and the checked
 * transfers on them. The calls are synchronous and run in the caller's task; read and write
 * buffers must be 64-byte aligned (the card driver does DMA on them).
 *
 * A disk driver fills a struct gendisk from alloc_disk() and registers it with add_disk(); the
 * whole disk is "part0" and the partitions are "<disk>p<N>". Access goes through a struct
 * block_device opened by name.
 */

#ifndef CARDSIGHT_BLKDEV_H
#define CARDSIGHT_BLKDEV_H

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <hal/device.h>

#define DISK_NAME_LEN  16 /* including the terminating zero */
#define BDEV_LABEL_LEN 32

#define GENHD_FL_RDONLY 0x02 /* write and discard are refused */

struct gendisk;

struct block_device_operations {
	/* sector numbers are relative to the disk, the range was checked, the buffer is 64-byte aligned */
	int (*read)(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count);
	/* may be NULL on a disk with GENHD_FL_RDONLY */
	int (*write)(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count);
	/* write out what the disk holds back; NULL when it writes through */
	int (*flush)(struct gendisk *disk);
	/* the last reference is gone (it was deleted and nobody has it open): free private_data */
	void (*free_disk)(struct gendisk *disk);
};

struct block_device {
	struct device dev; /* the name is the disk name or "<disk>p<N>" */
	struct gendisk *bd_disk;
	uint32_t partno; /* 0: the whole disk */
	uint64_t start_sect; /* first sector in the disk */
	uint64_t nr_sects;
	uint8_t part_type; /* the type byte of the partition table, 0 for a disk or without one */
	bool read_only;
	char label[BDEV_LABEL_LEN];
	uint32_t openers; /* bdev_open_by_name() without bdev_close() yet */
	uint32_t mounts;  /* file systems mounted from it */
	struct block_device *next;
};

struct gendisk {
	char disk_name[DISK_NAME_LEN]; /* set by the driver before add_disk() */
	void *private_data;
	const struct block_device_operations *fops;
	uint64_t capacity; /* sectors of logical_block_size bytes */
	uint32_t logical_block_size; /* 512 */
	uint32_t flags;
	struct block_device *part0; /* the whole disk */
	struct block_device *parts; /* the partitions, in partition number order */
	uint32_t part_count;
	struct gendisk *next; /* the registered disks */
	bool registered;
	bool dead; /* del_gendisk() ran: every access fails with -ENODEV */
};

/* A zeroed disk with its whole-disk block device; NULL when out of memory. */
struct gendisk *alloc_disk(void);

/* Drop the caller's reference (after del_gendisk(), or after a failed add_disk()). */
void put_disk(struct gendisk *disk);

/*
 * Register the disk and read its partition table (MBR or GPT; a table that cannot be read is
 * logged, the disk stays). -EINVAL for a bad description, -EEXIST when the name exists.
 */
int add_disk(struct gendisk *disk);

/* Take the disk and its partitions away. -EBUSY while one of them is mounted. */
int del_gendisk(struct gendisk *disk);

/* The same without the -EBUSY, for a card that is gone. */
void del_gendisk_force(struct gendisk *disk);

/* A registered block device by name ("mmcblk0", "mmcblk0p1", "/dev/..."): a reference or NULL. */
struct block_device *bdev_open_by_name(const char *name);
void bdev_close(struct block_device *bdev);

int bdev_read(struct block_device *bdev, uint64_t sector, void *buffer, uint32_t count);
int bdev_write(struct block_device *bdev, uint64_t sector, const void *buffer, uint32_t count);
int bdev_flush(struct block_device *bdev);

bool bdev_read_only(const struct block_device *bdev);
uint64_t bdev_nr_sectors(const struct block_device *bdev);
uint32_t bdev_sector_size(const struct block_device *bdev);
bool bdev_is_whole(const struct block_device *bdev);
const char *bdev_name(const struct block_device *bdev);

/* A file system keeps a device claimed from its mount to its unmount. */
int bdev_claim_mount(struct block_device *bdev);
void bdev_release_mount(struct block_device *bdev);

/* ---- partition tables ---- */

enum blk_partition_table_type {
	BLK_PARTITION_TABLE_MBR,
	BLK_PARTITION_TABLE_GPT,
};

enum blk_partition_content {
	BLK_PARTITION_FAT32,
	BLK_PARTITION_EXFAT,
	BLK_PARTITION_EXT4,
};

struct blk_partition_layout_entry {
	uint64_t sectors; /* 0: the rest of the disk */
	enum blk_partition_content content;
	char label[BDEV_LABEL_LEN];
};

/*
 * Write a fresh table (MBR or GPT) and rescan it: 1 MiB alignment, at most four entries for the
 * MBR. The first entry starts at sector 2048. -ENOSPC when the sizes do not fit, -EBUSY while
 * a partition is mounted.
 */
/* Erase the MBR and both GPT copies, leaving the card without a partition table. */
int blk_erase_partition_table(struct gendisk *disk);

int blk_write_partition_layout(struct gendisk *disk, enum blk_partition_table_type table,
	const struct blk_partition_layout_entry *entries, uint32_t count);

#endif /* CARDSIGHT_BLKDEV_H */
