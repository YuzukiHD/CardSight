// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_dir_load.c
 * @brief First-load indexing of one source directory.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Populate the entry cache of a directory.
 *
 * @details The extent was sized by the count pass. The load pass reads the
 * source directory, skips entries an overlay tombstone hides, adopts the slot
 * and extent of every entry the host changed, appends host-created entries and
 * finally assigns contiguous data extents to the remaining source files. The
 * result is sorted by slot so a sector renders in one binary search.
 *
 * @param[in] identifier Directory identifier.
 * @return 0 on success, or a negative errno.
 */
int lazy_dir_load(uint32_t identifier)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[identifier];
	struct vfs_dirent *entry = NULL;
	struct vfs_dir *handle = NULL;
	char path[VFS_PATH_MAX];
	uint32_t maximum;
	uint32_t capacity;
	uint32_t names_capacity;
	uint32_t cache_bytes;
	uint32_t slot_cursor = 0U;
	uint32_t count = 0U;
	uint32_t needed = 0U;
	uint32_t cursor;
	uint32_t overlay_records;
	int error = 0;
	int result;

	if (dir->cache_ready) {
		lazy_lru_touch(identifier);
		return 0;
	}
	if (dir->is_root) {
		dir->cache_ready = true;
		return 0;
	}
	if (lazy_cancel_requested()) {
		return -ECANCELED;
	}
	result = lazy_dir_reserve_extent(identifier, 0U);
	if (result != 0) {
		return result;
	}
	if (dir->source_error != 0) {
		dir->cache_ready = true;
		dir->entry_count = 0U;
		return 0;
	}
	maximum = dir->planned_entries;
	if (maximum > lazy.dir_max_entries) {
		maximum = lazy.dir_max_entries;
	}
	overlay_records = lazy_overlay_dir_count(identifier);
	capacity = maximum + overlay_records +
		CONFIG_STORAGE_PROXY_LAZY_DIR_ENTRY_SLACK;
	if (capacity > lazy.cache_entries_max) {
		capacity = lazy.cache_entries_max;
	}
	if (capacity < maximum) {
		/* The whole directory cannot fit the global cache; expose what fits. */
		maximum = capacity;
		dir->truncated = true;
		lazy.truncated = true;
	}
	if (capacity == 0U) {
		dir->data_ready = true;
		dir->data_range_ready = true;
		dir->entry_count = 0U;
		dir->cache_bytes = 0U;
		dir->cache_ready = true;
		lazy_lru_touch(identifier);
		return 0;
	}
	names_capacity = dir->planned_name_bytes;
	if (names_capacity < 512U) {
		names_capacity = 512U;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		const struct proxy_lazy_overlay *record = &lazy.overlay[index];

		if (record->used && record->dir_id == identifier) {
			names_capacity += (uint32_t)strlen(lazy.overlay_names +
				record->name_offset) + 1U;
		}
	}
	if (names_capacity > CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES) {
		names_capacity = CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES;
	}
	cache_bytes = capacity * (uint32_t)sizeof(struct proxy_lazy_dirent) + names_capacity;
	result = lazy_cache_make_room(cache_bytes, capacity);
	if (result != 0) {
		lazy.truncated = true;
		lazy.last_error = result;
		return result;
	}
	dir->entries = lazy_calloc(capacity, sizeof(*dir->entries));
	dir->names = lazy_alloc(names_capacity);
	if (dir->entries == NULL || dir->names == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	dir->entry_capacity = capacity;
	dir->name_capacity = names_capacity;
	dir->name_used = 0U;
	result = lazy_dir_path(identifier, path, sizeof(path));
	if (result != 0) {
		goto fail;
	}
	handle = vfs_opendir(path, &error);
	if (handle == NULL) {
		result = error != 0 ? error : -EIO;
		goto fail;
	}
	entry = lazy_alloc(sizeof(*entry));
	if (entry == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	result = vfs_readdir(handle, entry);
	while (result == 0 && count < maximum) {
		struct proxy_lazy_dirent *item;
		struct proxy_lazy_overlay *record = overlay_records != 0U ?
			lazy_overlay_find_name(identifier, entry->name) : NULL;
		uint32_t name_offset;

		if (record != NULL && record->deleted) {
			/* The host deleted it; a placeholder is added below. */
			result = vfs_readdir(handle, entry);
			continue;
		}
		if (lazy_dir_name_copy(dir, entry->name, &name_offset) != 0) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
		item = &dir->entries[count];
		memset(item, 0, sizeof(*item));
		item->name_offset = name_offset;
		item->entries = (uint16_t)lazy_name_entries(entry->name);
		item->directory = entry->st.dir;
		if (record != NULL) {
			item->slot = record->slot;
			item->first_cluster = record->first_cluster;
			item->clusters = record->clusters;
			item->size = record->size;
			item->directory = record->directory;
			item->host_owned = true;
			item->fat_chain = record->fat_chain;
		} else {
			/*
			 * Without a host overlay the source order is the slot order, so
			 * the next slot is simply the cursor. The general helper scans
			 * every entry already added and every overlay record for each
			 * new entry, which is quadratic on a directory with a thousand
			 * files.
			 */
			if (overlay_records == 0U) {
				item->slot = slot_cursor;
				slot_cursor += item->entries;
			} else {
				item->slot = lazy_free_slot(identifier, dir, count,
					item->entries, slot_cursor);
				slot_cursor = item->slot + item->entries;
			}
			if (slot_cursor < item->slot) {
				slot_cursor = item->entries;
			}
		}
		if (item->directory) {
			uint32_t child;

			result = lazy_dir_child(dir, entry->name, entry->st.alloc_size, &child);
			if (result == -ENOSPC) {
				dir->truncated = true;
				lazy.truncated = true;
				result = 0;
				break;
			}
			if (result != 0) {
				goto fail;
			}
			if (!item->host_owned) {
				item->first_cluster = lazy.dirs[child].first_cluster;
				item->clusters = lazy.dirs[child].clusters;
			}
			item->fat_chain = lazy.dirs[child].chained;
		} else if (!item->host_owned) {
			uint64_t clusters = (entry->st.size + PROXY_LAZY_CLUSTER_SIZE - 1U) /
				PROXY_LAZY_CLUSTER_SIZE;

			if (clusters > UINT32_MAX) {
				result = -EFBIG;
				goto fail;
			}
			item->size = entry->st.size;
			item->clusters = (uint32_t)clusters;
		}
		count++;
		if (count % PROXY_LAZY_PROGRESS_INTERVAL == 0U) {
			lazy_progress_report(count, path);
			if (lazy_cancel_requested()) {
				result = -ECANCELED;
				goto fail;
			}
		}
		result = vfs_readdir(handle, entry);
	}
	if (result == 0) {
		dir->truncated = true;
		lazy.truncated = true;
	} else if (result != -ENOENT) {
		goto fail;
	}
	vfs_closedir(handle);
	handle = NULL;
	lazy_free(entry);
	entry = NULL;
	dir->entry_count = count;
	/* Entries only the overlay knows about: host-created objects. */
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		const struct proxy_lazy_overlay *record = &lazy.overlay[index];
		struct proxy_lazy_dirent *item;
		const char *name;
		uint32_t name_offset;

		if (!record->used || record->dir_id != identifier) {
			continue;
		}
		if (count >= capacity) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
		name = lazy.overlay_names + record->name_offset;
		if (!record->deleted &&
		    lazy_dir_find_name(dir, name, true) != UINT32_MAX) {
			continue;
		}
		if (lazy_dir_name_copy(dir, name, &name_offset) != 0) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
		item = &dir->entries[count];
		memset(item, 0, sizeof(*item));
		item->name_offset = name_offset;
		item->entries = (uint16_t)lazy_name_entries(name);
		item->slot = record->slot;
		item->directory = record->directory;
		item->deleted = record->deleted;
		if (!record->deleted) {
			item->first_cluster = record->first_cluster;
			item->clusters = record->clusters;
			item->size = record->size;
			item->host_owned = true;
			item->fat_chain = record->fat_chain;
		}
		count++;
		if (count >= capacity) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
	}
	lazy_dir_sort(dir);
	/* Clamp overlapping groups forward: the host must never see two owners of a slot. */
	{
		uint32_t used = 0U;

		for (uint32_t index = 0U; index < count; index++) {
			struct proxy_lazy_dirent *item = &dir->entries[index];

			if (index != 0U && item->slot < used) {
				item->slot = used;
				dir->truncated = true;
				lazy.truncated = true;
			}
			used = item->slot + item->entries;
		}
	}
	/* Hand out contiguous data clusters for the files the source owns. */
	for (uint32_t index = 0U; index < count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if (!item->directory && !item->deleted && !item->host_owned) {
			needed += item->clusters;
		}
	}
	if (!dir->data_ready || needed > dir->data_clusters) {
		result = lazy_alloc_clusters(dir->partition, needed, &dir->data_first_cluster,
			&dir->data_clusters);
		if (result != 0) {
			goto fail;
		}
		dir->data_ready = true;
	}
	cursor = dir->data_first_cluster;
	for (uint32_t index = 0U; index < count; index++) {
		struct proxy_lazy_dirent *item = &dir->entries[index];

		if (item->directory || item->deleted || item->host_owned || item->clusters == 0U) {
			continue;
		}
		if ((uint64_t)cursor + item->clusters >
		    (uint64_t)dir->data_first_cluster + dir->data_clusters) {
			result = -EIO;
			goto fail;
		}
		item->first_cluster = cursor;
		cursor += item->clusters;
	}
	dir->entry_count = count;
	dir->data_range_ready = true;
	result = lazy_rebuild_data_ranges(identifier);
	if (result != 0) {
		goto fail;
	}
	/* Account for the capacity the load actually ended up with. */
	cache_bytes = dir->entry_capacity * (uint32_t)sizeof(struct proxy_lazy_dirent) +
		dir->name_capacity;
	dir->cache_bytes = cache_bytes;
	lazy.cache_entries += count;
	lazy.cache_bytes += cache_bytes;
	dir->cache_ready = true;
	lazy_lru_touch(identifier);
	return 0;
fail:
	if (handle != NULL) {
		vfs_closedir(handle);
	}
	lazy_free(entry);
	lazy_free(dir->entries);
	lazy_free(dir->names);
	dir->entries = NULL;
	dir->names = NULL;
	dir->entry_count = 0U;
	dir->entry_capacity = 0U;
	dir->name_capacity = 0U;
	dir->name_used = 0U;
	if (result != -ECANCELED) {
		pr_warn("lazy proxy: load %s failed: %d\n", lazy_dir_name(identifier), result);
	}
	lazy.last_error = result;
	return result;
}
