/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file layout.c
 * @brief Writing a fresh partition table (MBR or GPT) and rescanning it.
 *
 * The first partition starts at sector 2048 (1 MiB) and every size is rounded down to a
 * multiple of it; an entry with sectors == 0 takes the rest of the disk. The MBR holds four
 * entries; the GPT keeps its 128-entry array at LBA 2 with the backup at the end of the disk.
 */

/** @brief Log module name used by the pr_*() helpers. */
#define LOG_MODULE "block"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bits.h>
#include <log.h>

#include "block/block_internal.h"


/** @brief First sector a partition may start at: 1 MiB, the usual alignment. */
#define LAYOUT_FIRST_SECTOR 2048U
/** @brief Granularity every start and length is aligned to, in sectors. */
#define LAYOUT_ALIGN_SECTORS 2048U
/** @brief Entries in the GPT entry array. */
#define GPT_ENTRY_COUNT	     128U
/** @brief Size of one GPT entry in bytes. */
#define GPT_ENTRY_SIZE	     128U
/** @brief Sectors the whole GPT entry array occupies. */
#define GPT_ENTRY_SECTORS    ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + BLOCK_SECTOR_SIZE - 1U) / BLOCK_SECTOR_SIZE)
/** @brief The first 512 KiB: boot code plus the table. */
#define ERASE_HEAD_SECTORS   1024U
/** @brief Size of the GPT header before its 4-byte CRC field. */
#define GPT_HEADER_SIZE	     92U

/** @brief One-sector scratch buffer for a table being written; the block lock protects it. */
static uint8_t layout_sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(64)));
/** @brief Scratch buffer for the GPT entry array; the block lock protects it. */
static uint8_t layout_entries[GPT_ENTRY_COUNT * GPT_ENTRY_SIZE] __attribute__((aligned(64)));

/** @brief GPT type GUID of a basic data partition (FAT32). */
static const uint8_t gpt_basic_data[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
/** @brief GPT type GUID of a Linux filesystem partition. */
static const uint8_t gpt_linux_filesystem[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
	0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

/**
 * @brief Compute the reflected CRC-32 used by GPT.
 *
 * @details Bitwise, which is fast enough for a few hundred bytes of table and needs no table
 * of its own. The polynomial is the standard 0xedb88320, the initial value all ones and the
 * result is inverted at the end, as the GPT specification requires.
 *
 * @param[in] data   Bytes to checksum.
 * @param[in] length Number of bytes.
 * @return The CRC-32 value.
 */
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

/**
 * @brief MBR type byte for a partition content.
 *
 * @param[in] content What the partition will hold.
 * @return 0x07 for exFAT, 0x83 for ext4, 0x0c (FAT32 LBA) otherwise.
 */
static uint8_t mbr_type(enum blk_partition_content content)
{
	switch (content) {
	case BLK_PARTITION_EXFAT: return 0x07;
	case BLK_PARTITION_EXT4: return 0x83;
	case BLK_PARTITION_FAT32:
	default: return 0x0c;
	}
}

/**
 * @brief Work out where every entry starts and how long it is, in sectors.
 *
 * @details Starts at the first usable sector rounded up to the alignment, then hands out each
 * entry in order. A non-zero size is rounded down to the alignment and rejected when it rounds
 * to zero; a zero size takes everything that is left, also aligned down. An entry that does not
 * fit in what remains fails with -ENOSPC.
 *
 * @param[in]  disk        Disk being laid out, used for its capacity.
 * @param[in]  entries     Requested entries.
 * @param[in]  count       Number of entries.
 * @param[in]  first_usable First sector the layout may use.
 * @param[in]  last_usable Last sector the layout may use.
 * @param[out] starts      Receives the first sector of each entry.
 * @param[out] lengths     Receives the length of each entry.
 * @return 0 on success, -EINVAL when a size rounds to zero, or -ENOSPC when the entries do not
 *         fit.
 */
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
			/* no size given: the rest of the disk, aligned down */
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

/**
 * @brief Write an MBR with up to four primary partitions.
 *
 * @details Builds the entries in the scratch sector, using the CHS placeholder 0xfe/0xff
 * values the spec uses for "too large for CHS" and 32-bit LBA fields, then writes sector 0.
 *
 * @param[in] disk    Disk to write the table to.
 * @param[in] entries Requested partitions.
 * @param[in] count   Number of entries, 1 to 4.
 * @return 0 on success, -EINVAL for a bad count, -ENOSPC when the disk is too small or the
 *         entries do not fit, or a write error.
 */
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

		/* the CHS fields are not used on an SD card: mark them out of range */
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

/**
 * @brief Derive a stable GUID for a disk or one of its partitions.
 *
 * @details The GPT wants unique identifiers and the SoC has no random source at this point, so
 * the GUID is a hash of the disk name and the entry index. The variant and version bits of the
 * first word are set so the value looks like a real UUID; the same disk always gets the same
 * GUIDs, which keeps a rewrite of the table comparable.
 *
 * @param[in]  disk  Disk the GUID belongs to.
 * @param[in]  index 0 for the disk itself, entry number for a partition.
 * @param[out] guid  Receives the 16-byte GUID.
 */
static void gpt_guid(const struct gendisk *disk, uint32_t index, uint8_t guid[16])
{
	uint32_t hash = 2166136261U;
	const char *name = disk->disk_name;

	/* FNV-1a over the disk name, then mixed with the entry index */
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

/**
 * @brief Store a partition label as UTF-16LE in a GPT entry.
 *
 * @details At most 36 UTF-16 code units fit; the field is left zero-filled past the end of the
 * string, which is the GPT terminator.
 *
 * @param[out] entry GPT entry receiving the label at offset 0x38.
 * @param[in]  label NUL-terminated label.
 */
static void gpt_label(uint8_t *entry, const char *label)
{
	for (uint32_t index = 0U; index < 36U && label[index] != '\0'; index++) {
		blk_put_le16(entry + 0x38U + index * 2U, (uint8_t)label[index]);
	}
}

/**
 * @brief Build one GPT header sector.
 *
 * @details Fills the fixed fields: signature, revision, header size, the current and backup
 * LBAs, the usable range, the disk GUID, the entry array location and the entry-array CRC.
 * The header's own CRC is computed last, with the checksum field still zero.
 *
 * @param[in]  disk         Disk the header describes.
 * @param[out] header       Sector buffer receiving the header.
 * @param[in]  current_lba  LBA of this header.
 * @param[in]  backup_lba   LBA of the other header.
 * @param[in]  entries_lba  LBA of this header's entry array.
 * @param[in]  first_usable First sector usable by partitions.
 * @param[in]  last_usable  Last sector usable by partitions.
 * @param[in]  entries_crc  CRC-32 of the entry array.
 */
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

/**
 * @brief Write a GPT with up to eight partitions and a protective MBR.
 *
 * @details The entry array is built once, with a type GUID, a hashed unique GUID, the sector
 * range and the label per entry, and checksummed. The backup copy is written first, then the
 * primary one, then the two headers, so an interrupted write leaves the old table recognisable
 * as invalid rather than half valid. The protective MBR is written last; its single 0xee entry
 * covers the disk, clamped to 32 bits.
 *
 * @param[in] disk    Disk to write the table to.
 * @param[in] entries Requested partitions.
 * @param[in] count   Number of entries, 1 to 8.
 * @return 0 on success, -EINVAL for a bad count, -ENOSPC when the disk is too small or the
 *         entries do not fit, or a write error.
 */
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
		/* the protective MBR: one 0xee entry covering the whole disk */
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

/**
 * @brief Erase the partition table and the boot area.
 *
 * @details Zeroes the first 512 KiB (some cards carry a boot image in front of the table and
 * the SoC would try to run it) in chunks of the GPT entry array size, then the backup GPT
 * entry array and header at the end of the disk, flushes, and rescans. The file data itself is
 * left where it is. The whole operation runs under the block lock and refuses a mounted disk.
 *
 * @param[in] disk Disk to blank.
 * @return 0 on success, -ENODEV when the disk is not registered, -ENOTSUP for a sector size
 *         other than 512 bytes, -EBUSY when a partition is mounted, or a write error.
 */
int blk_erase_partition_table(struct gendisk *disk)
{
	int result = 0;

	if (disk == NULL || !disk->registered) {
		return -ENODEV;
	}
	if (disk->logical_block_size != BLOCK_SECTOR_SIZE) {
		return -ENOTSUP;
	}
	blk_lock();
	if (disk_has_mounts(disk)) {
		result = -EBUSY;
	} else {
		memset(layout_entries, 0, GPT_ENTRY_SECTORS * BLOCK_SECTOR_SIZE);
		{
			/* MBR, GPT, and any boot code the card carries at its front. */
			uint64_t head = disk->capacity < ERASE_HEAD_SECTORS ? disk->capacity :
				ERASE_HEAD_SECTORS;

			for (uint64_t sector = 0U; result == 0 && sector < head;
				sector += GPT_ENTRY_SECTORS) {
				uint32_t chunk = head - sector < GPT_ENTRY_SECTORS ?
					(uint32_t)(head - sector) : GPT_ENTRY_SECTORS;

				result = bdev_write(disk->part0, sector, layout_entries, chunk);
			}
		}
		if (result == 0 && disk->capacity > GPT_ENTRY_SECTORS + 3U) {
			/* backup entry array and its header, at the end of the disk */
			uint64_t tail = disk->capacity - (GPT_ENTRY_SECTORS + 1U);

			result = bdev_write(disk->part0, tail, layout_entries, GPT_ENTRY_SECTORS);
			if (result == 0) {
				result = bdev_write(disk->part0, disk->capacity - 1U, layout_entries, 1U);
			}
		}
		if (result == 0) {
			result = bdev_flush(disk->part0);
		}
		if (result == 0) {
			/* a blank disk has no table: the scan returns -ENOENT, not an error */
			blk_drop_partitions(disk);
			result = blk_scan_partitions(disk);
			if (result == -ENOENT) {
				result = 0;
			}
		}
	}
	blk_unlock();
	return result;
}

/**
 * @brief Write a fresh partition table and rescan it.
 *
 * @details Refuses an unregistered disk, a sector size other than 512 bytes, an unknown table
 * type and a disk with a mounted partition. The table is written under the block lock, flushed
 * and scanned again, so the new partitions appear with the names the table implies.
 *
 * @param[in] disk    Disk to lay out.
 * @param[in] table   MBR or GPT.
 * @param[in] entries Requested partitions.
 * @param[in] count   Number of entries.
 * @return 0 on success, -ENODEV, -ENOTSUP, -EINVAL or -EBUSY on refusal, or a write error.
 */
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
