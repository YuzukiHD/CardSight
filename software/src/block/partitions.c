/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Partition tables: the MBR at sector 0 (four primary entries) and the GPT behind a protective
 * MBR (header at LBA 1, entry array where the header says). The partitions become block devices
 * "<disk>p<N>"; the GPT keeps its entry order.
 */

#define LOG_MODULE "block"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <log.h>

#include "block/block_internal.h"


#define MBR_TABLE_OFFSET 446U
#define GPT_ENTRIES_SCAN 4096U
#define GPT_MAX_ENTRIES	 128U

static const uint8_t gpt_basic_data[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
static const uint8_t gpt_linux_filesystem[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
	0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
static const uint8_t gpt_efi_system[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
	0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

/* The table sector and the GPT entry window; the block lock is held while they are in use. */
static uint8_t table_sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(64)));
static uint8_t gpt_window[GPT_ENTRIES_SCAN] __attribute__((aligned(64)));

static void gpt_label(const uint8_t *entry, char label[BDEV_LABEL_LEN])
{
	const uint8_t *utf16 = entry + 0x38U;

	label[0] = '\0';
	for (unsigned int index = 0U; index + 1U < BDEV_LABEL_LEN - 1U && index < 36U; index += 2U) {
		uint16_t character = blk_le16(utf16 + index);

		if (character == 0U) {
			break;
		}
		label[index / 2U] = character < 0x80U ? (char)character : '?';
		label[index / 2U + 1U] = '\0';
	}
}

static uint8_t gpt_part_type(const uint8_t *type)
{
	if (memcmp(type, gpt_basic_data, 16U) == 0) {
		return 0x0c; /* FAT32 (LBA): what a data partition is called here */
	}
	if (memcmp(type, gpt_linux_filesystem, 16U) == 0) {
		return 0x83;
	}
	if (memcmp(type, gpt_efi_system, 16U) == 0) {
		return 0xef;
	}
	return 0x00;
}

static int add_partition(struct gendisk *disk, unsigned int partno, uint64_t start, uint64_t sectors,
	uint8_t type, bool read_only, const char *label)
{
	struct block_device *bdev = calloc(1, sizeof(*bdev));
	struct block_device **link = &disk->parts;

	if (bdev == NULL) {
		return -ENOMEM;
	}
	device_initialize(&bdev->dev);
	bdev->dev.release = bdev_release;
	bdev->dev.parent = &disk->part0->dev;
	bdev->bd_disk = disk;
	bdev->partno = partno;
	bdev->start_sect = start;
	bdev->nr_sects = sectors;
	bdev->part_type = type;
	bdev->read_only = read_only;
	if (label != NULL) {
		snprintf(bdev->label, sizeof(bdev->label), "%s", label);
	}
	snprintf(bdev->dev.name, sizeof(bdev->dev.name), "%sp%u", disk->disk_name, partno);
	get_device(&disk->part0->dev); /* the partition's reference on its disk */
	while (*link != NULL) {
		link = &(*link)->next;
	}
	*link = bdev;
	disk->part_count++;
	return 0;
}

static int scan_mbr(struct gendisk *disk)
{
	int result = 0;

	for (unsigned int slot = 0U; slot < 4U; slot++) {
		const uint8_t *entry = table_sector + MBR_TABLE_OFFSET + slot * 16U;
		uint8_t type = entry[4];
		uint64_t start = blk_le32(entry + 8U);
		uint64_t sectors = blk_le32(entry + 12U);

		if (type == 0x00U || type == 0x05U || type == 0x0fU || type == 0x85U ||
		    start == 0U || sectors == 0U) {
			continue;
		}
		if (start > disk->capacity || sectors > disk->capacity - start) {
			continue;
		}
		result = add_partition(disk, slot + 1U, start, sectors, type, false, NULL);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

static int scan_gpt(struct gendisk *disk)
{
	uint64_t entries_lba;
	uint32_t count;
	uint32_t entry_size;
	uint64_t sectors_in_window;
	unsigned int entries;
	int result;

	if (disk->fops->read(disk, 1U, table_sector, 1U) != 0 || memcmp(table_sector, "EFI PART", 8U) != 0) {
		return -ENOENT;
	}
	entries_lba = blk_le64(table_sector + 0x48U);
	count = blk_le32(table_sector + 0x50U);
	entry_size = blk_le32(table_sector + 0x54U);
	if (count == 0U || count > GPT_MAX_ENTRIES || entry_size < 128U) {
		return -EINVAL;
	}
	sectors_in_window = (GPT_ENTRIES_SCAN + BLOCK_SECTOR_SIZE - 1U) / BLOCK_SECTOR_SIZE;
	if (entries_lba > disk->capacity || sectors_in_window > disk->capacity - entries_lba) {
		return -EINVAL;
	}
	if (disk->fops->read(disk, entries_lba, gpt_window, (uint32_t)sectors_in_window) != 0) {
		return -EIO;
	}
	entries = GPT_ENTRIES_SCAN / entry_size;
	if (entries > count) {
		entries = count;
	}
	for (unsigned int index = 0U; index < entries; index++) {
		const uint8_t *entry = gpt_window + (size_t)index * entry_size;
		uint8_t type[16];
		uint64_t start;
		uint64_t end;
		char label[BDEV_LABEL_LEN];

		memset(type, 0, sizeof(type));
		if (memcmp(entry, type, 16U) == 0) {
			continue;
		}
		start = blk_le64(entry + 0x20U);
		end = blk_le64(entry + 0x28U);
		if (end < start || start >= disk->capacity) {
			continue;
		}
		if (end >= disk->capacity) {
			end = disk->capacity - 1U;
		}
		gpt_label(entry, label);
		result = add_partition(disk, index + 1U, start, end - start + 1U,
			gpt_part_type(entry), false, label);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

int blk_scan_partitions(struct gendisk *disk)
{
	bool protective = false;

	if (disk->fops->read(disk, 0U, table_sector, 1U) != 0) {
		return -EIO;
	}
	if (table_sector[510] != 0x55U || table_sector[511] != 0xaaU) {
		return -ENOENT;
	}
	for (unsigned int slot = 0U; slot < 4U; slot++) {
		if (table_sector[MBR_TABLE_OFFSET + slot * 16U + 4U] == 0xeeU) {
			protective = true;
		}
	}
	return protective ? scan_gpt(disk) : scan_mbr(disk);
}

void blk_drop_partitions(struct gendisk *disk)
{
	struct block_device *bdev = disk->parts;

	disk->parts = NULL;
	disk->part_count = 0U;
	while (bdev != NULL) {
		struct block_device *next = bdev->next;

		bdev->dev.parent = NULL;
		put_device(&bdev->dev);
		bdev = next;
	}
}

int blkdev_rescan_partitions(struct gendisk *disk)
{
	int result;

	blk_lock();
	if (!disk->registered) {
		result = -ENODEV;
	} else if (disk->logical_block_size != BLOCK_SECTOR_SIZE) {
		result = -ENOTSUP;
	} else if (disk_has_mounts(disk)) {
		result = -EBUSY;
	} else {
		blk_drop_partitions(disk);
		result = blk_scan_partitions(disk);
		if (result == -ENOENT) {
			result = 0;
		}
	}
	blk_unlock();
	return result;
}
