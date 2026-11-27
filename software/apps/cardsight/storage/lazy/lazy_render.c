// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_render.c
 * @brief exFAT entry encoding and the synthesized root reports.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- exFAT entry rendering ---- */

/**
 * @brief Build one raw exFAT directory entry of an object.
 *
 * @details Entry zero is the stream entry, entry one is the stream extension
 * with length, hash, first cluster and both data lengths, and the remaining
 * entries are the name characters in 15-code-unit chunks.
 *
 * @param[in]  name          UTF-8 object name.
 * @param[in]  directory     True for a directory.
 * @param[in]  first_cluster First cluster of the object's extent.
 * @param[in]  clusters      Number of clusters of a directory extent.
 * @param[in]  size          File size in bytes.
 * @param[in]  fat_chain     True when the extent is a FAT chain.
 * @param[in]  position      Entry position inside the object's group.
 * @param[out] entry         Buffer receiving the 32-byte entry.
 */
void lazy_entry_raw(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain, uint32_t position, uint8_t entry[32])
{
	uint16_t characters[VFS_NAME_MAX + 1];
	size_t length = lazy_utf16(name, characters, VFS_NAME_MAX);
	uint32_t entries = 2U + (uint32_t)((length + 14U) / 15U);
	uint64_t data_length = directory ? (uint64_t)clusters * PROXY_LAZY_CLUSTER_SIZE : size;

	memset(entry, 0, 32U);
	if (position == 0U) {
		entry[0] = 0x85U;
		entry[1] = (uint8_t)(entries - 1U);
		/* The host may write every object: no read-only attribute. */
		lazy_put_le16(entry + 4U, directory ? 0x10U : 0x20U);
	} else if (position == 1U) {
		entry[0] = 0xc0U;
		/* AllocationPossible, and NoFatChain unless the extent is a chain. */
		entry[1] = fat_chain ? 0x01U : 0x03U;
		entry[3] = (uint8_t)length;
		lazy_put_le16(entry + 4U, lazy_name_hash(characters, length));
		lazy_put_le64(entry + 8U, data_length);
		lazy_put_le32(entry + 20U, first_cluster);
		lazy_put_le64(entry + 24U, data_length);
	} else {
		size_t start = (size_t)(position - 2U) * 15U;

		entry[0] = 0xc1U;
		for (uint32_t character = 0U; character < 15U; character++) {
			size_t name_index = start + character;

			lazy_put_le16(entry + 2U + character * 2U,
				name_index < length ? characters[name_index] : 0U);
		}
	}
}

/**
 * @brief Compute the checksum of an object's whole entry group.
 *
 * @param[in] name          UTF-8 object name.
 * @param[in] directory     True for a directory.
 * @param[in] first_cluster First cluster of the object's extent.
 * @param[in] clusters      Number of clusters of a directory extent.
 * @param[in] size          File size in bytes.
 * @param[in] fat_chain     True when the extent is a FAT chain.
 * @return The entry-set checksum.
 */
uint16_t lazy_entry_checksum(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain)
{
	uint8_t entry[32];
	uint32_t entries = lazy_name_entries(name);
	uint16_t checksum = 0U;

	for (uint32_t position = 0U; position < entries; position++) {
		lazy_entry_raw(name, directory, first_cluster, clusters, size, fat_chain,
			position, entry);
		for (uint32_t index = 0U; index < sizeof(entry); index++) {
			if (position == 0U && (index == 2U || index == 3U)) {
				continue;
			}
			checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
				(checksum >> 1) + entry[index]);
		}
	}
	return checksum;
}

/**
 * @brief Build one directory entry of an object, with its checksum filled in.
 *
 * @param[in]  name          UTF-8 object name.
 * @param[in]  directory     True for a directory.
 * @param[in]  first_cluster First cluster of the object's extent.
 * @param[in]  clusters      Number of clusters of a directory extent.
 * @param[in]  size          File size in bytes.
 * @param[in]  fat_chain     True when the extent is a FAT chain.
 * @param[in]  position      Entry position inside the object's group.
 * @param[out] entry         Buffer receiving the 32-byte entry.
 */
void lazy_render_entry(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain, uint32_t position, uint8_t entry[32])
{
	lazy_entry_raw(name, directory, first_cluster, clusters, size, fat_chain, position,
		entry);
	if (position == 0U) {
		lazy_put_le16(entry + 2U, lazy_entry_checksum(name, directory, first_cluster,
			clusters, size, fat_chain));
	}
}

/**
 * @brief Render one of the three fixed system entries of the root directory.
 *
 * @param[in]  position System entry index.
 * @param[out] entry    Buffer receiving the 32-byte entry.
 */
void lazy_root_system_entry(uint32_t position, uint8_t entry[32])
{
	/** @brief Volume label stored in the root directory. */
	static const uint16_t label[] = { 'C', 'a', 'r', 'd', 'S', 'i', 'g', 'h', 't' };

	memset(entry, 0, 32U);
	if (position == 0U) {
		entry[0] = 0x83U;
		entry[1] = sizeof(label) / sizeof(label[0]);
		for (uint32_t index = 0U; index < sizeof(label) / sizeof(label[0]); index++) {
			lazy_put_le16(entry + 2U + index * 2U, label[index]);
		}
	} else if (position == 1U) {
		entry[0] = 0x81U;
		lazy_put_le32(entry + 20U, lazy.bitmap_cluster);
		lazy_put_le64(entry + 24U, lazy.bitmap_bytes);
	} else {
		entry[0] = 0x82U;
		lazy_put_le32(entry + 4U, lazy.upcase_checksum);
		lazy_put_le32(entry + 20U, lazy.upcase_cluster);
		lazy_put_le64(entry + 24U, lazy.upcase_bytes);
	}
}

/**
 * @brief Find the cached entry that occupies a slot.
 *
 * @details Entries are stored in readdir order and their slots increase
 * monotonically, so a binary search finds the group.
 *
 * @param[in] dir    Cached directory.
 * @param[in] wanted Absolute slot number.
 * @return The entry index, or UINT32_MAX.
 */
uint32_t lazy_entry_by_slot(const struct proxy_lazy_dir *dir, uint32_t wanted)
{
	uint32_t low = 0U;
	uint32_t high = dir->entry_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const struct proxy_lazy_dirent *item = &dir->entries[middle];

		if (wanted < item->slot) {
			high = middle;
		} else if (wanted >= item->slot + item->entries) {
			low = middle + 1U;
		} else {
			return middle;
		}
	}
	return UINT32_MAX;
}

/**
 * @brief Return the file name of a synthesized root report.
 *
 * @param[in] kind One of @ref proxy_lazy_root_kind that is not a folder.
 * @return The constant file name.
 */
const char *lazy_root_file_name(uint32_t kind)
{
	return kind == PROXY_LAZY_ROOT_LAYOUT ? "_LAYOUT.TXT" : "_DIR_LIMIT.TXT";
}

/**
 * @brief Render a root directory slot, reserving a partition folder if needed.
 *
 * @details System entries and partition folders are synthesized exactly as
 * before. The two informational files are rendered as ordinary read-only files
 * whose data comes from @c lazy_root_text().
 *
 * @param[in]  slot  Absolute slot number.
 * @param[out] entry Buffer receiving the 32-byte entry, or zeros.
 * @return 0 on success, or a negative errno.
 */
int lazy_render_root_slot(uint32_t slot, uint8_t entry[32])
{
	memset(entry, 0, 32U);
	if (slot < PROXY_LAZY_ROOT_SYSTEM_SLOTS) {
		lazy_root_system_entry(slot, entry);
		return 0;
	}
	for (uint32_t index = 0U; index < lazy.root_entry_count; index++) {
		const struct proxy_lazy_root_entry *root_entry = &lazy.root_entries[index];

		if (slot >= root_entry->slot && slot < root_entry->slot + root_entry->entries) {
			if (root_entry->kind != PROXY_LAZY_ROOT_FOLDER) {
				lazy_render_entry(lazy_root_file_name(root_entry->kind), false,
					root_entry->first_cluster, root_entry->clusters,
					PROXY_LAZY_ROOT_TEXT_BYTES, false,
					slot - root_entry->slot, entry);
				return 0;
			}
			{
				struct proxy_lazy_dir *dir = &lazy.dirs[root_entry->dir_id];
				int result = lazy_dir_reserve_extent(root_entry->dir_id, 0U);

				if (result != 0) {
					return result;
				}
				lazy_render_entry(lazy_dir_name(root_entry->dir_id), true,
					dir->first_cluster, dir->clusters, 0U, dir->chained,
					slot - root_entry->slot, entry);
			}
			return 0;
		}
	}
	return 0;
}

/**
 * @brief Render one sector of a view directory.
 *
 * @param[in]  identifier Directory identifier.
 * @param[in]  offset     Byte offset of the sector inside the directory extent.
 * @param[out] sector     Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
int lazy_render_directory(uint32_t identifier, uint64_t offset,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	uint32_t first_slot = (uint32_t)(offset / 32U);
	int result = 0;

	memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
	if (identifier == PROXY_LAZY_ROOT_ID) {
		for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
			result = lazy_render_root_slot(first_slot + slot,
				sector + slot * 32U);
			if (result != 0) {
				return result;
			}
		}
		return 0;
	}
	result = lazy_dir_load(identifier);
	if (result != 0) {
		return result;
	}
	{
		const struct proxy_lazy_dir *dir = &lazy.dirs[identifier];
		uint32_t last_slot = dir->entry_count != 0U ?
			dir->entries[dir->entry_count - 1U].slot : 0U;

		for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
			uint32_t absolute = first_slot + slot;
			uint32_t index = lazy_entry_by_slot(dir, absolute);

			if (index != UINT32_MAX) {
				const struct proxy_lazy_dirent *item = &dir->entries[index];

				lazy_render_entry(dir->names + item->name_offset, item->directory,
					item->first_cluster, item->clusters, item->size,
					item->fat_chain, absolute - item->slot,
					sector + slot * 32U);
				if (item->deleted) {
					/* exFAT marks a deleted entry by clearing its in-use bit. */
					sector[slot * 32U] &= 0x7fU;
				}
			} else if (last_slot > absolute) {
				/*
				 * A free slot before a live entry must not read as zero: zero is
				 * the end-of-directory marker. A deleted stream entry says "skip me".
				 */
				sector[slot * 32U] = 0x05U;
			}
		}
	}
	return 0;
}

/* ---- synthesized root reports ---- */

/**
 * @brief Append one formatted line to a synthesized report.
 *
 * @details The formatter's terminator is replaced by a newline so the next
 * append continues on the following line.
 *
 * @param[out] buffer   Destination buffer.
 * @param[in]  capacity Size of @p buffer in bytes.
 * @param[in]  used     Bytes already written.
 * @param[in]  format   printf format of the line.
 * @param[in]  ...      Arguments for @p format.
 * @return The new used byte count, clamped to @p capacity.
 */
size_t lazy_text_add(char *buffer, size_t capacity, size_t used,
	const char *format, ...)
{
	va_list arguments;
	size_t end;
	int written;

	if (used >= capacity) {
		return capacity;
	}
	va_start(arguments, format);
	written = vsnprintf(buffer + used, capacity - used, format, arguments);
	va_end(arguments);
	if (written < 0) {
		return used;
	}
	end = used + (size_t)written;
	if (end < capacity) {
		buffer[end++] = '\n'; /* Replace the terminator the formatter wrote. */
	}
	return end < capacity ? end : capacity;
}

/**
 * @brief Render the whole text of one synthesized root report.
 *
 * @details The report is regenerated on every read so it reflects the current
 * directory count, cache usage and truncation state. The buffer is pre-filled
 * with newlines, so the unused tail is whitespace rather than a NUL terminator
 * and the file size stays constant.
 *
 * @param[in]  kind     One of @ref proxy_lazy_root_kind that is not a folder.
 * @param[out] buffer   Destination buffer.
 * @param[in]  capacity Size of @p buffer in bytes; the whole buffer is the file.
 * @return The file size, which is @p capacity.
 */
uint32_t lazy_root_text(uint32_t kind, char *buffer, size_t capacity)
{
	size_t used = 0U;

	memset(buffer, '\n', capacity);
	if (kind == PROXY_LAZY_ROOT_LAYOUT) {
		uint32_t reserved = 0U;

		for (uint32_t index = 0U; index < PROXY_LAZY_PARTITIONS; index++) {
			if (lazy.partitions[index].selected) {
				reserved += lazy.partitions[index].reserve_clusters;
			}
		}
		used = lazy_text_add(buffer, capacity, used, "CardSight layout");
		used = lazy_text_add(buffer, capacity, used, "cluster_bytes=%u",
			(uint32_t)PROXY_LAZY_CLUSTER_SIZE);
		used = lazy_text_add(buffer, capacity, used, "total_clusters=%u",
			lazy.total_clusters);
		used = lazy_text_add(buffer, capacity, used, "reserved_clusters=%u", reserved);
		used = lazy_text_add(buffer, capacity, used, "tail_first=%u", lazy.tail_first);
		used = lazy_text_add(buffer, capacity, used, "tail_clusters=%u",
			lazy.tail_clusters);
		used = lazy_text_add(buffer, capacity, used, "directories=%u", lazy.dir_count);
		used = lazy_text_add(buffer, capacity, used, "cache_entries=%u",
			lazy.cache_entries);
		used = lazy_text_add(buffer, capacity, used, "cache_entries_max=%u",
			lazy.cache_entries_max);
		used = lazy_text_add(buffer, capacity, used, "cache_bytes=%u", lazy.cache_bytes);
		used = lazy_text_add(buffer, capacity, used, "cache_bytes_max=%u",
			lazy.cache_bytes_max);
		used = lazy_text_add(buffer, capacity, used, "dir_max_entries=%u",
			lazy.dir_max_entries);
		used = lazy_text_add(buffer, capacity, used, "truncated=%u",
			lazy.truncated ? 1U : 0U);
		for (uint32_t index = 0U; index < PROXY_LAZY_PARTITIONS; index++) {
			const struct proxy_lazy_partition *part = &lazy.partitions[index];

			if (!part->selected) {
				continue;
			}
			used = lazy_text_add(buffer, capacity, used, "p%u_reserved_first=%u",
				index + 1U, part->reserve_first);
			used = lazy_text_add(buffer, capacity, used, "p%u_reserved_clusters=%u",
				index + 1U, part->reserve_clusters);
		}
	} else {
		used = lazy_text_add(buffer, capacity, used, "CardSight limits");
		used = lazy_text_add(buffer, capacity, used, "dir_max_entries=%u",
			lazy.dir_max_entries);
		used = lazy_text_add(buffer, capacity, used, "proxy.lazy.truncated=%u",
			lazy.truncated ? 1U : 0U);
		used = lazy_text_add(buffer, capacity, used, "directories=%u", lazy.dir_count);
	}
	(void)used;
	return (uint32_t)capacity;
}

/**
 * @brief Fill one sector of a synthesized root report.
 *
 * @param[in]  cluster           View cluster being read.
 * @param[in]  sector_in_cluster Sector offset inside the cluster.
 * @param[out] sector            Buffer receiving the sector.
 * @return One when the cluster belongs to a report, zero otherwise.
 */
int lazy_root_text_read(uint32_t cluster, uint32_t sector_in_cluster,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	for (uint32_t index = 0U; index < lazy.root_entry_count; index++) {
		const struct proxy_lazy_root_entry *entry = &lazy.root_entries[index];
		uint64_t offset;

		if (entry->kind == PROXY_LAZY_ROOT_FOLDER ||
		    cluster < entry->first_cluster ||
		    cluster >= entry->first_cluster + entry->clusters) {
			continue;
		}
		offset = (uint64_t)(cluster - entry->first_cluster) *
			PROXY_LAZY_CLUSTER_SIZE +
			(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;
		memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
		if (offset == 0U) {
			/* The report is exactly one sector; later sectors read as zeros. */
			lazy_root_text(entry->kind, (char *)sector, PROXY_LAZY_SECTOR_SIZE);
		}
		return 1;
	}
	return 0;
}

/**
 * @brief Report whether a view cluster belongs to a synthesized root report.
 *
 * @param[in] cluster Cluster number.
 * @return True when the report owns the cluster.
 */
bool lazy_root_text_contains(uint32_t cluster)
{
	for (uint32_t index = 0U; index < lazy.root_entry_count; index++) {
		const struct proxy_lazy_root_entry *entry = &lazy.root_entries[index];

		if (entry->kind != PROXY_LAZY_ROOT_FOLDER &&
		    cluster >= entry->first_cluster &&
		    cluster < entry->first_cluster + entry->clusters) {
			return true;
		}
	}
	return false;
}
