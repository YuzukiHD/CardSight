// SPDX-License-Identifier: GPL-2.0-or-later
/* A root text file that describes the source disk: its partition table and where each partition sits. */

#include "proxy_internal.h"

#include <bits.h>
#include <size.h>
#include <stdarg.h>

#define LAYOUT_TEXT_MAX 2048U
/* The MBR is one sector; a GPT entry array of 128 entries is eight sectors. */
#define LAYOUT_TABLE_SECTORS 8U

/* The proxy lock is held for the whole start, so one set of scratch buffers serves the builder. */
static uint8_t layout_table[LAYOUT_TABLE_SECTORS * PROXY_SECTOR_SIZE] __attribute__((aligned(64)));
static uint8_t layout_array[LAYOUT_TABLE_SECTORS * PROXY_SECTOR_SIZE] __attribute__((aligned(64)));
static char layout_text[LAYOUT_TEXT_MAX];

struct layout_entry {
	uint64_t start;
	uint64_t sectors;
	uint8_t mbr_type;
	uint8_t gpt_type[16];
	bool bootable;
	bool valid;
};

static const uint8_t gpt_efi_system[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
	0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const uint8_t gpt_basic_data[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
static const uint8_t gpt_linux_filesystem[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
	0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
static const uint8_t gpt_linux_swap[16] = { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
	0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f };

static const char *mbr_type_name(uint8_t type)
{
	switch (type) {
	case 0x0b: return "FAT32 (CHS)";
	case 0x0c: return "FAT32 (LBA)";
	case 0x07: return "exFAT or NTFS";
	case 0x05: return "extended (CHS)";
	case 0x0f: return "extended (LBA)";
	case 0x82: return "Linux swap";
	case 0x83: return "Linux";
	case 0xee: return "GPT protective";
	default: return "unknown";
	}
}

static const char *gpt_type_name(const uint8_t type[16])
{
	static const struct {
		const uint8_t *guid;
		const char *name;
	} names[] = {
		{ gpt_efi_system, "EFI system" },
		{ gpt_basic_data, "Microsoft basic data" },
		{ gpt_linux_filesystem, "Linux filesystem" },
		{ gpt_linux_swap, "Linux swap" },
	};

	for (unsigned int index = 0U; index < ARRAY_SIZE(names); index++) {
		if (memcmp(type, names[index].guid, 16U) == 0) {
			return names[index].name;
		}
	}
	return "other";
}

/* GPT stores the first three fields of a GUID little endian. */
static void gpt_guid_text(const uint8_t guid[16], char output[37])
{
	snprintf(output, 37U, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		guid[3], guid[2], guid[1], guid[0], guid[5], guid[4], guid[7], guid[6],
		guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
		(uint32_t)bytes[3] << 24;
}

static uint64_t read_le64(const uint8_t *bytes)
{
	uint64_t value = 0U;

	for (unsigned int index = 0U; index < 8U; index++) {
		value |= (uint64_t)bytes[index] << (index * 8U);
	}
	return value;
}

static void text_append(char *text, size_t size, size_t *used, const char *format, ...)
{
	va_list arguments;
	int written;

	if (*used >= size) {
		return;
	}
	va_start(arguments, format);
	written = vsnprintf(text + *used, size - *used, format, arguments);
	va_end(arguments);
	if (written > 0) {
		*used += (size_t)written < size - *used ? (size_t)written : size - *used - 1U;
	}
}

/* The on-disk table itself, so the file describes partitions the view does not expose as well. */
static bool read_table(struct block_device *source, struct layout_entry entries[PROXY_PARTITIONS + 4U],
	bool *gpt, char gpt_header_text[16])
{
	uint8_t *table = layout_table;
	unsigned int found = 0U;

	if (bdev_read(source, 0U, table, 1U) != 0 || table[510] != 0x55U || table[511] != 0xaaU) {
		return false;
	}
	*gpt = table[450] == 0xeeU;
	if (!*gpt) {
		for (unsigned int slot = 0U; slot < 4U; slot++) {
			const uint8_t *entry = table + 446U + slot * 16U;

			if (entry[4] == 0U) {
				continue;
			}
			entries[found].start = ((uint64_t)entry[11] << 24) | ((uint64_t)entry[10] << 16) |
				((uint64_t)entry[9] << 8) | entry[8];
			entries[found].sectors = ((uint64_t)entry[15] << 24) | ((uint64_t)entry[14] << 16) |
				((uint64_t)entry[13] << 8) | entry[12];
			entries[found].mbr_type = entry[4];
			entries[found].bootable = entry[0] == 0x80U;
			entries[found].valid = true;
			found++;
		}
		return true;
	}
	/* GPT: header at LBA 1 points at the entry array. */
	if (bdev_read(source, 1U, table, LAYOUT_TABLE_SECTORS) != 0 || memcmp(table, "EFI PART", 8U) != 0) {
		return false;
	}
	{
		uint64_t array_lba = read_le64(table + 0x48U);
		uint32_t count = read_le32(table + 0x50U);
		uint32_t entry_size = read_le32(table + 0x54U);
		uint8_t *array = layout_array;
		uint32_t available;

		if (count == 0U || count > 128U || entry_size < 128U ||
			bdev_read(source, array_lba, array, LAYOUT_TABLE_SECTORS) != 0) {
			return false;
		}
		/*
		 * Only the sectors read above can be looked at. A full 128-entry array spans 32 sectors;
		 * the partitions the view can expose are looked for in the first ones.
		 */
		available = LAYOUT_TABLE_SECTORS * PROXY_SECTOR_SIZE / entry_size;
		if (available > count) {
			available = count;
		}
		snprintf(gpt_header_text, 16U, "%llu", (unsigned long long)array_lba);
		for (uint32_t index = 0U; index < available && found < PROXY_PARTITIONS + 4U; index++) {
			const uint8_t *entry = array + (size_t)index * entry_size;

			if (memcmp(entry, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16U) == 0) {
				continue;
			}
			entries[found].start = read_le64(entry + 0x20U);
			entries[found].sectors = read_le64(entry + 0x28U) - entries[found].start + 1U;
			memcpy(entries[found].gpt_type, entry, 16U);
			entries[found].valid = true;
			found++;
		}
	}
	return true;
}

static const struct proxy_partition *exposed_partition(uint64_t start)
{
	for (unsigned int index = 0U; index < PROXY_PARTITIONS; index++) {
		const struct proxy_partition *part = &proxy.partitions[index];

		if (part->present && part->start_sector == start) {
			return part;
		}
	}
	return NULL;
}

int proxy_layout_add_file(void)
{
	struct layout_entry entries[PROXY_PARTITIONS + 4U];
	struct block_device *source;
	char *text = layout_text;
	char gpt_header_text[16] = "2";
	char guid[37];
	size_t used = 0U;
	bool gpt = false;
	bool table_read;

	source = bdev_open_by_name(proxy.source_name);
	if (source == NULL) {
		return -ENODEV;
	}
	memset(entries, 0, sizeof(entries));
	table_read = read_table(source, entries, &gpt, gpt_header_text);
	{
		char capacity[16];

		format_capacity(bdev_nr_sectors(source) * 512U, capacity, sizeof(capacity));
		text_append(text, LAYOUT_TEXT_MAX, &used,
			"CardSight proxy disk layout\r\n\r\n"
			"disk      %s\r\n"
			"capacity  %llu sectors (%s)\r\n",
			proxy.source_name, (unsigned long long)bdev_nr_sectors(source), capacity);
	}
	bdev_close(source);
	if (!table_read) {
		text_append(text, LAYOUT_TEXT_MAX, &used, "table     none (no MBR or GPT signature)\r\n");
	} else if (gpt) {
		text_append(text, LAYOUT_TEXT_MAX, &used, "table     GPT (header at LBA 1, entries at LBA %s)\r\n",
			gpt_header_text);
	} else {
		text_append(text, LAYOUT_TEXT_MAX, &used, "table     MBR (partition table at offset 446)\r\n");
	}
	{
		char view[16];

		format_capacity(proxy.total_sectors * 512U, view, sizeof(view));
		text_append(text, LAYOUT_TEXT_MAX, &used,
			"view      %s, %s exFAT, %s\r\n\r\n"
			"#   start        end          sectors      size        type                              exposed  filesystem\r\n",
			PROXY_DISK_NAME, view, proxy.read_only ? "read only" : "writable");
	}
	for (unsigned int index = 0U; index < ARRAY_SIZE(entries); index++) {
		const struct layout_entry *entry = &entries[index];
		const struct proxy_partition *part;
		char kind[64];

		if (!entry->valid) {
			continue;
		}
		if (gpt) {
			gpt_guid_text(entry->gpt_type, guid);
			snprintf(kind, sizeof(kind), "%.30s %.36s", gpt_type_name(entry->gpt_type), guid);
		} else {
			snprintf(kind, sizeof(kind), "0x%02x %s%s", entry->mbr_type,
				mbr_type_name(entry->mbr_type), entry->bootable ? " bootable" : "");
		}
		part = exposed_partition(entry->start);
		text_append(text, LAYOUT_TEXT_MAX, &used, "p%-3u%-12llu %-12llu %-12llu %-11llu %-33s %-8s %s%s%s\r\n",
			index + 1U, (unsigned long long)entry->start,
			(unsigned long long)(entry->start + entry->sectors - 1U),
			(unsigned long long)entry->sectors,
			(unsigned long long)(entry->sectors * 512U / 1000000U), kind,
			part != NULL ? "yes" : "no",
			part != NULL && part->status == 0 ? part->volume.fstype : "-",
			part != NULL && part->volume.label[0] != '\0' ? " " : "",
			part != NULL ? part->volume.label : "");
	}
	text_append(text, LAYOUT_TEXT_MAX, &used, "\r\nWritten by CardSight when the view was started.\r\n");
	return proxy_add_message(0U, "_LAYOUT.TXT", text);
}
