/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file partitions.c
 * @brief Partition tables: the MBR at sector 0 and the GPT behind a protective MBR.
 *
 * The MBR has four primary entries; the GPT sits behind a protective MBR (header at LBA 1,
 * entry array where the header says). The partitions become block devices "<disk>p<N>"; the
 * GPT keeps its entry order.
 */

/** @brief Log module name used by the pr_*() helpers. */
#define LOG_MODULE "block"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <log.h>

#include "block/block_internal.h"


/** @brief Offset of the four 16-byte MBR entries inside sector 0. */
#define MBR_TABLE_OFFSET 446U
/** @brief Bytes of the GPT entry array read in one window. */
#define GPT_ENTRIES_SCAN 4096U
/** @brief Highest GPT entry count the scanner accepts. */
#define GPT_MAX_ENTRIES	 128U

/** @brief GPT type GUID of a basic data partition (FAT32). */
static const uint8_t gpt_basic_data[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
/** @brief GPT type GUID of a Linux filesystem partition. */
static const uint8_t gpt_linux_filesystem[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
	0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
/** @brief GPT type GUID of an EFI system partition. */
static const uint8_t gpt_efi_system[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
	0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

/* The table sector and the GPT entry window; the block lock is held while they are in use. */
/** @brief One sector of scratch space for the table being scanned. */
static uint8_t table_sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(64)));
/** @brief Window holding the GPT entry array. */
static uint8_t gpt_window[GPT_ENTRIES_SCAN] __attribute__((aligned(64)));

/**
 * @brief Decode the UTF-16LE label of a GPT entry.
 *
 * @details Reads code units until a terminator, the 36-unit limit or the output buffer is
 * full. Characters outside ASCII are replaced by '?', because the block device label field is
 * a plain byte string.
 *
 * @param[in]  entry GPT entry, label at offset 0x38.
 * @param[out] label Buffer of @c BDEV_LABEL_LEN bytes receiving a NUL-terminated label.
 */
static void gpt_label(const uint8_t *entry, char label[BDEV_LABEL_LEN])
{
	const uint8_t *utf16 = entry + 0x38U;

	label[0] = '\0';
	for (uint32_t index = 0U; index + 1U < BDEV_LABEL_LEN - 1U && index < 36U; index += 2U) {
		uint16_t character = blk_le16(utf16 + index);

		if (character == 0U) {
			break;
		}
		label[index / 2U] = character < 0x80U ? (char)character : '?';
		label[index / 2U + 1U] = '\0';
	}
}

/**
 * @brief Map a GPT type GUID to the MBR type byte the rest of the layer uses.
 *
 * @param[in] type 16-byte type GUID of the entry.
 * @return 0x0c for a basic data partition, 0x83 for a Linux filesystem, 0xef for an EFI system
 *         partition, or 0x00 for any other type.
 */
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

/**
 * @brief Allocate one partition block device and append it to its disk.
 *
 * @details The device starts with one reference of its own, points at the disk and takes an
 * extra reference on the disk so it outlives a removed disk. The name becomes "<disk>p<N>"
 * and the entry is appended to keep the table's order.
 *
 * @param[in] disk      Disk the partition belongs to.
 * @param[in] partno    Partition number, starting at 1.
 * @param[in] start     First sector inside the disk.
 * @param[in] sectors   Length in sectors.
 * @param[in] type      Type byte reported to the UI.
 * @param[in] read_only true when the entry must refuse writes.
 * @param[in] label     Volume label, or NULL.
 * @return 0 on success, or -ENOMEM when the record cannot be allocated.
 */
static int add_partition(struct gendisk *disk, uint32_t partno, uint64_t start, uint64_t sectors,
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

/**
 * @brief Register the primary entries of the MBR already in @c table_sector.
 *
 * @details Walks the four entries. Empty entries, extended-partition entries (0x05, 0x0f,
 * 0x85) and entries with no start or length are skipped, and so is an entry that runs past the
 * end of the disk; the chain is not followed, so only primary partitions appear.
 *
 * @param[in] disk Disk being scanned.
 * @return 0 on success, or -ENOMEM from add_partition().
 */
static int scan_mbr(struct gendisk *disk)
{
	int result = 0;

	for (uint32_t slot = 0U; slot < 4U; slot++) {
		const uint8_t *entry = table_sector + MBR_TABLE_OFFSET + slot * 16U;
		uint8_t type = entry[4];
		uint64_t start = blk_le32(entry + 8U);
		uint64_t sectors = blk_le32(entry + 12U);

		/* unused, extended, or empty entries are not partitions of their own */
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

/**
 * @brief Register the entries of the GPT that follows the protective MBR.
 *
 * @details Reads LBA 1 and checks the "EFI PART" signature, then takes the entry array address,
 * count and size from the header. The window is read in one transfer; the entry count is
 * clamped to what fits and to the count in the header. Unused entries are skipped, an end
 * beyond the disk is clamped, and the label and type GUID are decoded for each partition.
 *
 * @param[in] disk Disk being scanned.
 * @return 0 on success, -ENOENT when there is no GPT header, -EINVAL for an implausible or
 *         out-of-range header, -EIO on a read error, or -ENOMEM from add_partition().
 */
static int scan_gpt(struct gendisk *disk)
{
	uint64_t entries_lba;
	uint32_t count;
	uint32_t entry_size;
	uint64_t sectors_in_window;
	uint32_t entries;
	int result;

	/* the header signature tells a real GPT from a stray 0xee type byte */
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
	for (uint32_t index = 0U; index < entries; index++) {
		const uint8_t *entry = gpt_window + (size_t)index * entry_size;
		uint8_t type[16];
		uint64_t start;
		uint64_t end;
		char label[BDEV_LABEL_LEN];

		/* an all-zero type GUID marks an unused slot */
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

/**
 * @brief Read the table of @p disk and register its partitions.
 *
 * @details Reads sector 0 once. No 0x55aa signature means no table. A type 0xee entry in any
 * MBR slot is the protective marker a GPT leaves behind, so the scan continues with the GPT
 * header; every other table is taken as an MBR.
 *
 * @param[in] disk Disk to scan.
 * @return 0 on success, -EIO when sector 0 cannot be read, -ENOENT without a table, or another
 *         negative errno from the table scanner.
 */
int blk_scan_partitions(struct gendisk *disk)
{
	bool protective = false;

	if (disk->fops->read(disk, 0U, table_sector, 1U) != 0) {
		return -EIO;
	}
	if (table_sector[510] != 0x55U || table_sector[511] != 0xaaU) {
		return -ENOENT;
	}
	/* any 0xee entry means the real table is a GPT further down the disk */
	for (uint32_t slot = 0U; slot < 4U; slot++) {
		if (table_sector[MBR_TABLE_OFFSET + slot * 16U + 4U] == 0xeeU) {
			protective = true;
		}
	}
	return protective ? scan_gpt(disk) : scan_mbr(disk);
}

/**
 * @brief Release every partition of a disk.
 *
 * @details The list is detached first, so a concurrent scan cannot see a half-released list.
 * Each record drops its parent link and its reference; the disk's reference taken by
 * add_partition() is released by bdev_release() when the last one goes.
 *
 * @param[in] disk Disk whose partitions go away.
 */
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

/**
 * @brief Drop and rescan the partitions of a registered disk.
 *
 * @details Takes the block lock, refuses a disk that is gone, one with a sector size other
 * than 512 bytes, and one with a mounted partition. The partitions are dropped and the table
 * read again; a disk without a table is a valid result, reported as success.
 *
 * @param[in] disk Disk to rescan.
 * @return 0 on success, -ENODEV, -ENOTSUP or -EBUSY on refusal, or a read error.
 */
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
