/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Writing a fresh partition table (MBR or GPT) and rescanning it.
 *
 * The first partition starts at sector 2048 (1 MiB) and every size is rounded down to a multiple
 * of it; an entry with sectors == 0 takes the rest of the disk. The MBR holds four entries; the
 * GPT keeps its 128-entry array at LBA 2 with the backup at the end of the disk.
 */

#define LOG_MODULE "block"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bits.h>
#include <log.h>

#include "block/block_internal.h"


#define LAYOUT_FIRST_SECTOR 2048U
#define LAYOUT_ALIGN_SECTORS 2048U
#define GPT_ENTRY_COUNT	     128U
#define GPT_ENTRY_SIZE	     128U
#define GPT_ENTRY_SECTORS    ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + BLOCK_SECTOR_SIZE - 1U) / BLOCK_SECTOR_SIZE)
#define GPT_HEADER_SIZE	     92U

static uint8_t layout_sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(64)));
static uint8_t layout_entries[GPT_ENTRY_COUNT * GPT_ENTRY_SIZE] __attribute__((aligned(64)));

static const uint8_t gpt_basic_data[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
static const uint8_t gpt_linux_filesystem[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
	0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

static uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffU;

	while (length-- != 0U) {
		crc ^= *data++;
		for (uint32_t bit = 0U; bit < 8U; bit++) {
			crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
		}
	}
	return ~crc;
}

static uint8_t mbr_type(enum blk_partition_content content)
{
	switch (content) {
	case BLK_PARTITION_EXFAT: return 0x07;
	case BLK_PARTITION_EXT4: return 0x83;
	case BLK_PARTITION_FAT32:
	default: return 0x0c;
	}
}

/* Start and length of every entry, in sectors; -ENOSPC when they do not fit. */
static int layout_ranges(const struct gendisk *disk, const struct blk_partition_layout_entry *entries,
	uint32_t count, uint64_t first_usable, uint64_t last_usable, uint64_t starts[], uint64_t lengths[])
{
	uint64_t next = ALIGN_UP(first_usable, LAYOUT_ALIGN_SECTORS);

	for (uint32_t index = 0U; index < count; index++) {
		uint64_t length;

		if (entries[index].sectors != 0U) {
			length = entries[index].sectors & ~((uint64_t)LAYOUT_ALIGN_SECTORS - 1U);
			if (length == 0U) {
				return -EINVAL;
			}
		} else {
			if (last_usable + 1U <= next) {
				return -ENOSPC;
			}
			length = (last_usable + 1U - next) & ~((uint64_t)LAYOUT_ALIGN_SECTORS - 1U);
			if (length == 0U) {
				return -ENOSPC;
			}
		}
		if (length > last_usable + 1U - next) {
			return -ENOSPC;
		}
		starts[index] = next;
		lengths[index] = length;
		next += length;
	}
	return 0;
}

static int write_mbr(struct gendisk *disk, const struct blk_partition_layout_entry *entries,
	uint32_t count)
{
	uint64_t starts[4];
	uint64_t lengths[4];
	int result;

	if (count == 0U || count > 4U || disk->capacity <= LAYOUT_FIRST_SECTOR) {
		return count == 0U || count > 4U ? -EINVAL : -ENOSPC;
	}
	result = layout_ranges(disk, entries, count, LAYOUT_FIRST_SECTOR, disk->capacity - 1U, starts, lengths);
	if (result != 0) {
		return result;
	}
	memset(layout_sector, 0, sizeof(layout_sector));
	for (uint32_t index = 0U; index < count; index++) {
		uint8_t *entry = layout_sector + 446U + index * 16U;

		entry[1] = entry[5] = 0xfeU;
		entry[2] = entry[3] = entry[6] = entry[7] = 0xffU;
		entry[4] = mbr_type(entries[index].content);
		blk_put_le32(entry + 8U, (uint32_t)starts[index]);
		blk_put_le32(entry + 12U, (uint32_t)lengths[index]);
	}
	layout_sector[510] = 0x55U;
	layout_sector[511] = 0xaaU;
	return bdev_write(disk->part0, 0U, layout_sector, 1U);
}

static void gpt_guid(const struct gendisk *disk, uint32_t index, uint8_t guid[16])
{
	uint32_t hash = 2166136261U;
	const char *name = disk->disk_name;

	while (*name != '\0') {
		hash = (hash ^ (uint8_t)*name++) * 16777619U;
	}
	hash ^= index * 2654435761U;
	memset(guid, 0, 16U);
	blk_put_le32(guid, hash | 0x40000000U);
	blk_put_le32(guid + 4, hash ^ 0x9e3779b9U);
	blk_put_le32(guid + 8, ~hash);
	blk_put_le32(guid + 12, hash * 31U);
}

static void gpt_label(uint8_t *entry, const char *label)
{
	for (uint32_t index = 0U; index < 36U && label[index] != '\0'; index++) {
		blk_put_le16(entry + 0x38U + index * 2U, (uint8_t)label[index]);
	}
}

static void gpt_header(struct gendisk *disk, uint8_t header[BLOCK_SECTOR_SIZE], uint64_t current_lba,
	uint64_t backup_lba, uint64_t entries_lba, uint64_t first_usable, uint64_t last_usable,
	uint32_t entries_crc)
{
	uint8_t disk_guid[16];

	memset(header, 0, BLOCK_SECTOR_SIZE);
	memcpy(header, "EFI PART", 8U);
	blk_put_le32(header + 8U, 0x00010000U);
	blk_put_le32(header + 12U, GPT_HEADER_SIZE);
	blk_put_le64(header + 24U, current_lba);
	blk_put_le64(header + 32U, backup_lba);
	blk_put_le64(header + 40U, first_usable);
	blk_put_le64(header + 48U, last_usable);
	gpt_guid(disk, 0U, disk_guid);
	memcpy(header + 56U, disk_guid, 16U);
	blk_put_le64(header + 72U, entries_lba);
	blk_put_le32(header + 80U, GPT_ENTRY_COUNT);
	blk_put_le32(header + 84U, GPT_ENTRY_SIZE);
	blk_put_le32(header + 88U, entries_crc);
	/* the checksum is written with its own field zero, so it goes last */
	blk_put_le32(header + 16U, crc32_ieee(header, GPT_HEADER_SIZE));
}

static int write_gpt(struct gendisk *disk, const struct blk_partition_layout_entry *entries,
	uint32_t count)
{
	uint64_t backup_header_lba = disk->capacity - 1U;
	uint64_t backup_entries_lba = backup_header_lba - GPT_ENTRY_SECTORS;
	uint64_t first_usable = 2U + GPT_ENTRY_SECTORS;
	uint64_t last_usable = backup_entries_lba - 1U;
	uint64_t starts[GPT_ENTRY_COUNT];
	uint64_t lengths[GPT_ENTRY_COUNT];
	uint32_t entries_crc;
	int result;

	if (count == 0U || count > 8U || disk->capacity <= 2U * GPT_ENTRY_SECTORS + LAYOUT_ALIGN_SECTORS) {
		return count == 0U || count > 8U ? -EINVAL : -ENOSPC;
	}
	result = layout_ranges(disk, entries, count, LAYOUT_FIRST_SECTOR, last_usable, starts, lengths);
	if (result != 0) {
		return result;
	}
	memset(layout_entries, 0, sizeof(layout_entries));
	for (uint32_t index = 0U; index < count; index++) {
		uint8_t *entry = layout_entries + index * GPT_ENTRY_SIZE;
		uint8_t guid[16];

		memcpy(entry, entries[index].content == BLK_PARTITION_EXT4 ? gpt_linux_filesystem : gpt_basic_data, 16U);
		gpt_guid(disk, index + 1U, guid);
		memcpy(entry + 16U, guid, 16U);
		blk_put_le64(entry + 32U, starts[index]);
		blk_put_le64(entry + 40U, starts[index] + lengths[index] - 1U);
		gpt_label(entry, entries[index].label);
	}
	entries_crc = crc32_ieee(layout_entries, sizeof(layout_entries));

	/* the backup copy first, then the primary one */
	result = bdev_write(disk->part0, backup_entries_lba, layout_entries, GPT_ENTRY_SECTORS);
	if (result == 0) {
		gpt_header(disk, layout_sector, backup_header_lba, 1U, backup_entries_lba, first_usable, last_usable,
			entries_crc);
		result = bdev_write(disk->part0, backup_header_lba, layout_sector, 1U);
	}
	if (result == 0) {
		result = bdev_write(disk->part0, 2U, layout_entries, GPT_ENTRY_SECTORS);
	}
	if (result == 0) {
		gpt_header(disk, layout_sector, 1U, backup_header_lba, 2U, first_usable, last_usable, entries_crc);
		result = bdev_write(disk->part0, 1U, layout_sector, 1U);
	}
	if (result == 0) {
		memset(layout_sector, 0, sizeof(layout_sector));
		layout_sector[446U + 4U] = 0xeeU;
		layout_sector[446U + 1U] = layout_sector[446U + 5U] = 0xfeU;
		layout_sector[446U + 2U] = layout_sector[446U + 3U] = 0xffU;
		layout_sector[446U + 6U] = layout_sector[446U + 7U] = 0xffU;
		blk_put_le32(layout_sector + 446U + 8U, 1U);
		blk_put_le32(layout_sector + 446U + 12U,
			disk->capacity - 1U > 0xffffffffU ? 0xffffffffU : (uint32_t)(disk->capacity - 1U));
		layout_sector[510] = 0x55U;
		layout_sector[511] = 0xaaU;
		result = bdev_write(disk->part0, 0U, layout_sector, 1U);
	}
	return result;
}

int blk_write_partition_layout(struct gendisk *disk, enum blk_partition_table_type table,
	const struct blk_partition_layout_entry *entries, uint32_t count)
{
	int result;

	if (disk == NULL || entries == NULL || !disk->registered) {
		return -ENODEV;
	}
	if (disk->logical_block_size != BLOCK_SECTOR_SIZE) {
		return -ENOTSUP;
	}
	if (table > BLK_PARTITION_TABLE_GPT) {
		return -EINVAL;
	}
	blk_lock();
	if (disk_has_mounts(disk)) {
		result = -EBUSY;
	} else {
		result = table == BLK_PARTITION_TABLE_GPT ? write_gpt(disk, entries, count) :
			write_mbr(disk, entries, count);
		if (result == 0) {
			result = bdev_flush(disk->part0);
		}
		if (result == 0) {
			result = blk_scan_partitions(disk);
			if (result == -ENOENT) {
				result = 0;
			}
		}
	}
	blk_unlock();
	return result;
}
