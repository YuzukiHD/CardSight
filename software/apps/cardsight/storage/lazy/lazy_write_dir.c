// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_write_dir.c
 * @brief Directory sector parsing, split-group window and sector writes.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Read a little-endian 16-bit value.
 *
 * @param[in] bytes Two bytes in little-endian order.
 * @return The decoded value.
 */
uint16_t lazy_get_le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

/**
 * @brief Read a little-endian 32-bit value.
 *
 * @param[in] bytes Four bytes in little-endian order.
 * @return The decoded value.
 */
uint32_t lazy_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * @brief Read a little-endian 64-bit value.
 *
 * @param[in] bytes Eight bytes in little-endian order.
 * @return The decoded value.
 */
uint64_t lazy_get_le64(const uint8_t *bytes)
{
	return (uint64_t)lazy_get_le32(bytes) |
		((uint64_t)lazy_get_le32(bytes + 4U) << 32);
}

/**
 * @brief Parse one directory sector the host wrote and apply its entries.
 *
 * @details A window of [kept head of a cut group][new sector] lets an entry
 * group that straddles the sector edge be completed. Deletions are seen by
 * comparing the new sector with the previous rendering. Groups whose bytes did
 * not change are skipped, so a host that rewrites a whole sector only pays for
 * what it changed.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] offset Byte offset of the sector inside the directory extent.
 * @param[in] data   New sector contents.
 * @return 0 on success, or a negative errno.
 */
int lazy_write_directory(uint32_t dir_id, uint64_t offset,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint8_t *window = lazy.directory_window;
	uint8_t *previous = lazy.directory_previous;
	uint32_t touched[PROXY_LAZY_SLOTS_PER_SECTOR + 1U];
	uint32_t touched_count = 0U;
	uint32_t head_slots = 0U;
	uint32_t window_slots;
	int result = 0;

	if (lazy.split_slots != 0U && lazy.split_dir == dir_id &&
	    lazy.split_offset + PROXY_LAZY_SECTOR_SIZE == offset) {
		head_slots = lazy.split_slots;
		memcpy(window, lazy.split_head, (size_t)head_slots * 32U);
	}
	lazy.split_slots = 0U;
	memset(window + (size_t)head_slots * 32U, 0,
		sizeof(lazy.directory_window) - (size_t)head_slots * 32U);
	memcpy(window + (size_t)head_slots * 32U, data, PROXY_LAZY_SECTOR_SIZE);
	window_slots = head_slots + PROXY_LAZY_SLOTS_PER_SECTOR;
	result = lazy_render_directory(dir_id, offset, previous);
	if (result != 0) {
		return result;
	}
	for (uint32_t slot = 0U; slot < window_slots; slot++) {
		const uint8_t *entry = window + slot * 32U;

		if (slot >= head_slots && previous[(slot - head_slots) * 32U] == 0x85U &&
		    entry[0] == 0x05U) {
			result = lazy_delete_entry(dir_id,
				(uint32_t)(offset / 32U) + slot - head_slots);
			if (result != 0) {
				return result;
			}
		}
		if (entry[0] == 0x85U) {
			uint32_t secondaries = entry[1];
			const uint8_t *stream;
			char name[VFS_NAME_MAX + 1];
			bool directory;
			bool fat_chain;
			uint32_t first_cluster;
			uint64_t data_length;
			uint64_t valid_length;
			uint32_t host_slot;

			if (secondaries < 2U || secondaries > 18U) {
				continue;
			}
			if (slot + secondaries >= window_slots) {
				lazy.split_slots = window_slots - slot;
				lazy.split_dir = dir_id;
				lazy.split_offset = offset;
				memcpy(lazy.split_head, window + slot * 32U,
					(size_t)lazy.split_slots * 32U);
				break;
			}
			if (window[(slot + 1U) * 32U] != 0xc0U ||
			    !lazy_entry_name(window, slot, secondaries, name)) {
				continue;
			}
			if (slot >= head_slots &&
			    slot - head_slots + secondaries < PROXY_LAZY_SLOTS_PER_SECTOR &&
			    memcmp(window + slot * 32U,
				previous + (slot - head_slots) * 32U,
				(size_t)(secondaries + 1U) * 32U) == 0) {
				slot += secondaries;
				continue;
			}
			stream = window + (slot + 1U) * 32U;
			directory = (lazy_get_le16(entry + 4U) & 0x10U) != 0U;
			fat_chain = (stream[1] & 0x02U) == 0U;
			first_cluster = lazy_get_le32(stream + 20U);
			data_length = lazy_get_le64(stream + 24U);
			valid_length = lazy_get_le64(stream + 8U);
			/*
			 * DataLength is the file size the host sees; ValidDataLength only
			 * says how much of it was physically written. A host that creates
			 * or preallocates a file (fsutil createnew, a copy engine setting
			 * the final size) leaves ValidDataLength at zero while DataLength
			 * is already the final size, so clamping to the smaller value
			 * would expose a zero-byte file and lose the stream waiting in
			 * staging. The source file is truncated to DataLength instead;
			 * the unwritten range reads back as zeros, exactly as exFAT
			 * specifies between the two lengths.
			 */
			(void)valid_length;
			host_slot = (uint32_t)(offset / 32U) - head_slots + slot;
			result = lazy_sync_entry(dir_id, name, directory, first_cluster,
				data_length, fat_chain, host_slot);
			if (result != 0) {
				return result;
			}
			if (touched_count < sizeof(touched) / sizeof(touched[0])) {
				touched[touched_count++] = host_slot;
			}
			slot += secondaries;
		}
	}
	if (dir->ranges_dirty) {
		lazy_dir_sort(dir);
		result = lazy_rebuild_data_ranges(dir_id);
		if (result != 0) {
			return result;
		}
		result = lazy_claim_pending_dir(dir_id);
		if (result != 0) {
			return result;
		}
		result = lazy_claim_runs();
		if (result != 0) {
			return result;
		}
	}
	/* A host writes whole sectors; the source file keeps the host's exact size. */
	for (uint32_t index = 0U; index < touched_count; index++) {
		uint32_t entry_index = lazy_entry_by_slot(dir, touched[index]);

		if (entry_index == UINT32_MAX) {
			continue;
		}
		{
			const struct proxy_lazy_dirent *item = &dir->entries[entry_index];

			if (!item->directory && !item->deleted) {
				result = lazy_truncate_entry(dir_id, entry_index, item->size);

				if (result != 0) {
					return result;
				}
			}
		}
	}
	dir->sizes_dirty = false;
	return 0;
}

/**
 * @brief Report whether a cluster continues a directory through a host FAT chain.
 *
 * @param[in] cluster Cluster being written.
 * @return The directory identifier, or PROXY_LAZY_INVALID_DIR.
 */
uint32_t lazy_directory_extension(uint32_t cluster)
{
	for (uint32_t index = 1U; index < lazy.dir_count; index++) {
		struct proxy_lazy_dir *dir = &lazy.dirs[index];

		if (dir->is_root || !dir->extent_ready) {
			continue;
		}
		if (dir->extension_cluster != 0U &&
		    lazy_fat_next(dir->extension_cluster) == cluster) {
			return index;
		}
	}
	return PROXY_LAZY_INVALID_DIR;
}

/**
 * @brief Map one cluster the host appended to a directory.
 *
 * @param[in] dir_id  Directory identifier.
 * @param[in] cluster Cluster to map.
 * @return 0 on success, or a negative errno.
 */
int lazy_map_directory_cluster(uint32_t dir_id, uint32_t cluster)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	int result;

	if (dir->truncated) {
		/* The host is growing a directory whose later entries are invisible. */
		lazy.last_error = -ENOSPC;
		return -ENOSPC;
	}
	if (cluster != dir->first_cluster + dir->clusters) {
		dir->chained = true;
		lazy_dir_parent_chained(dir_id);
	}
	result = lazy_range_add(cluster, 1U, dir_id, PROXY_LAZY_RANGE_DIRECTORY,
		PROXY_LAZY_INVALID_SLOT, dir->clusters);

	if (result != 0) {
		return result;
	}
	lazy_pending_drop_cluster(cluster);
	dir->clusters++;
	dir->extension_cluster = cluster;
	return 0;
}

/**
 * @brief Apply one host sector write.
 *
 * @details The sector is routed by the volume area it belongs to: system areas
 * are read-only, the bitmap is updated in RAM, a directory sector is parsed, a
 * file sector is written through to its source file, and a sector in the free
 * tail waits in the pending table until its entry arrives.
 *
 * @param[in] sector_number Logical sector number inside the view.
 * @param[in] data          Sector contents.
 * @return 0 on success, or a negative errno.
 */
int lazy_write_sector(uint64_t sector_number, const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	struct proxy_lazy_range range;
	uint64_t relative;
	uint32_t cluster;
	uint16_t sector_in_cluster;

	if (sector_number >= lazy.total_sectors) {
		return -EINVAL;
	}
	if (sector_number < PROXY_LAZY_BOOT_SECTORS) {
		return 0; /* The geometry is fixed; volume flags are not used. */
	}
	if (sector_number < PROXY_LAZY_FAT_OFFSET) {
		return 0;
	}
	if (sector_number < PROXY_LAZY_FAT_OFFSET + lazy.fat_sectors) {
		int result = lazy_fat_write((uint32_t)(sector_number - PROXY_LAZY_FAT_OFFSET), data);

		return result;
	}
	if (sector_number < lazy.data_start) {
		return 0;
	}
	relative = sector_number - lazy.data_start;
	cluster = (uint32_t)(relative / PROXY_LAZY_SECTORS_PER_CLUSTER) +
		PROXY_LAZY_ROOT_CLUSTER_MIN;
	sector_in_cluster = (uint16_t)(relative % PROXY_LAZY_SECTORS_PER_CLUSTER);
	if (cluster >= lazy.bitmap_cluster && cluster < lazy.bitmap_cluster + lazy.bitmap_clusters) {
		uint64_t bitmap_offset = (uint64_t)(cluster - lazy.bitmap_cluster) *
			PROXY_LAZY_CLUSTER_SIZE +
			(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;
		size_t length = bitmap_offset < lazy.bitmap_bytes ?
			lazy.bitmap_bytes - (size_t)bitmap_offset : 0U;

		if (length > PROXY_LAZY_SECTOR_SIZE) {
			length = PROXY_LAZY_SECTOR_SIZE;
		}
		if (length != 0U) {
			for (size_t index = 0U; index < length; index++) {
				uint32_t byte_offset = (uint32_t)bitmap_offset + (uint32_t)index;

				for (uint32_t bit = 0U; bit < 8U; bit++) {
					uint32_t number = 2U + byte_offset * 8U + bit;
					bool used = (data[index] & (uint8_t)(1U << bit)) != 0U;
					bool was = (lazy.bitmap[byte_offset] &
						(uint8_t)(1U << bit)) != 0U;

					if (number < lazy.tail_first) {
						/* The reservations stay used no matter what the host wrote. */
						lazy.bitmap[byte_offset] |= (uint8_t)(1U << bit);
					} else if (number < lazy.total_clusters + 2U) {
						if (used) {
							lazy.bitmap[byte_offset] |= (uint8_t)(1U << bit);
						} else {
							lazy.bitmap[byte_offset] &= (uint8_t)~(1U << bit);
						}
						if (was && !used) {
							lazy_pending_drop_cluster(number);
							lazy_run_drop_cluster(number);
							lazy_range_remove_data_cluster(number);
							lazy_forget_freed_cluster(number);
						}
					} else {
						lazy.bitmap[byte_offset] &= (uint8_t)~(1U << bit);
					}
				}
			}
		}
		lazy.clean = false;
		return 0;
	}
	if (cluster >= lazy.upcase_cluster && cluster < lazy.upcase_cluster + lazy.upcase_clusters) {
		return -EROFS;
	}
	if (lazy_root_text_contains(cluster)) {
		/* The informational root files are generated; the host cannot change them. */
		return -EROFS;
	}
	if (lazy_range_find(cluster, &range) == 0) {
		if (range.kind == PROXY_LAZY_RANGE_DIRECTORY) {
			uint64_t offset = ((uint64_t)range.offset +
				(cluster - range.base)) * PROXY_LAZY_CLUSTER_SIZE +
				(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;

			if (range.dir_id == PROXY_LAZY_ROOT_ID) {
				uint8_t current[PROXY_LAZY_SECTOR_SIZE];
				uint32_t first_slot = (uint32_t)(offset / 32U);

				memset(current, 0, sizeof(current));
				for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
					int result = lazy_render_root_slot(first_slot + slot,
						current + slot * 32U);

					if (result != 0) {
						return result;
					}
				}
				/*
				 * The system and partition-folder slots are synthesized and stay
				 * read-only. Slots from root_host_slot on are the host's metadata
				 * area (System Volume Information and anything else Windows adds);
				 * accept those writes without persisting them, because the root
				 * entry is already rendered from a real backing directory.
				 */
				for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
					uint32_t absolute = first_slot + slot;

					if (absolute < lazy.root_host_slot &&
					    memcmp(current + slot * 32U, data + slot * 32U, 32U) != 0) {
						pr_warn("lazy: root system slot %u changed\n", absolute);
						return -EROFS;
					}
				}
				return 0;
			}
			return lazy_write_directory(range.dir_id, offset, data);
		}
		{
			int result = lazy_dir_load(range.dir_id);
			uint32_t index;

			if (result != 0) {
				return result;
			}
			index = lazy_entry_by_slot(&lazy.dirs[range.dir_id], range.slot);
			if (index == UINT32_MAX) {
				return -EIO;
			}
			return lazy_write_bytes(range.dir_id, index,
				((uint64_t)range.offset + (cluster - range.base)) *
				PROXY_LAZY_CLUSTER_SIZE +
				(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE,
				data, PROXY_LAZY_SECTOR_SIZE);
		}
	}
	/*
	 * A cluster the host appended to a directory is mapped when the parent
	 * entry reports the larger data length, or when the host wrote a FAT link
	 * from the directory's last cluster. Adjacency alone is not evidence: a
	 * host-created file's extent often starts right after a directory extent.
	 */
	{
		uint32_t dir_id = lazy_directory_extension(cluster);

		if (dir_id != PROXY_LAZY_INVALID_DIR) {
			int result = lazy_map_directory_cluster(dir_id, cluster);

			if (result != 0) {
				return result;
			}
			return lazy_write_directory(dir_id,
				(uint64_t)(lazy.dirs[dir_id].clusters - 1U), data);
		}
	}
	/*
	 * A cluster inside a reservation may be unowned because the host freed and
	 * reused it before rewriting the directory entry (truncate-then-write).
	 * Keep the data until an entry claims the extent; if none ever does, the
	 * bounded pending table drops it without touching a source file.
	 */
	return lazy_orphan_store(cluster, sector_in_cluster, data);
}
