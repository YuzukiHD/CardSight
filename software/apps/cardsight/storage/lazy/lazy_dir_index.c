// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_dir_index.c
 * @brief Entry slots, sorted caches, lookups and file-data range rebuilds.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/** @brief Entry array the first-cluster comparator sorts, set under the lock. */
static const struct proxy_lazy_dirent *lazy_sort_entries;


/* ---- indexing a directory ---- */

/**
 * @brief Compare two cached entries by slot.
 *
 * @param[in] left  First element, an index into @c lazy_sort_entries.
 * @param[in] right Second element, an index into @c lazy_sort_entries.
 * @return Negative, zero or positive as @p left sorts before, with or after @p right.
 */
int lazy_slot_compare(const void *left, const void *right)
{
	uint32_t left_index = *(const uint32_t *)left;
	uint32_t right_index = *(const uint32_t *)right;
	uint32_t left_slot = lazy_sort_entries[left_index].slot;
	uint32_t right_slot = lazy_sort_entries[right_index].slot;

	return left_slot < right_slot ? -1 : left_slot > right_slot ? 1 : 0;
}

/**
 * @brief Find an existing child header of a parent by name.
 *
 * @param[in] parent Parent directory.
 * @param[in] name   Child name.
 * @return The child identifier, or PROXY_LAZY_INVALID_DIR.
 */
uint32_t lazy_dir_find_child(const struct proxy_lazy_dir *parent, const char *name)
{
	for (uint32_t index = 0U; index < parent->subdir_count; index++) {
		uint32_t identifier = parent->subdirs[index];

		if (strcasecmp(lazy_dir_name(identifier), name) == 0) {
			return identifier;
		}
	}
	return PROXY_LAZY_INVALID_DIR;
}

/**
 * @brief Find or create the header of one child directory.
 *
 * @details Child headers are looked up by name, so a source directory that is
 * reordered between loads still maps to the extent it was given the first time.
 * A host-created directory already has its header and host extent.
 *
 * @param[in,out] parent Parent directory.
 * @param[in]     name   Child name.
 * @param[out]    child  Receives the child identifier.
 * @return 0 on success, or a negative errno.
 */
int lazy_dir_child(struct proxy_lazy_dir *parent, const char *name, uint64_t alloc_size,
	uint32_t *child)
{
	uint32_t identifier = lazy_dir_find_child(parent, name);
	int result;

	if (identifier == PROXY_LAZY_INVALID_DIR) {
		result = lazy_subdir_reserve(parent);
		if (result != 0) {
			return result;
		}
		identifier = lazy_dir_alloc(name);
		if (identifier == UINT32_MAX) {
			return -ENOSPC;
		}
		lazy.dirs[identifier].parent = (uint32_t)(parent - lazy.dirs);
		lazy.dirs[identifier].partition = parent->partition;
		result = lazy_subdir_append(parent, identifier);
		if (result != 0) {
			return result;
		}
	}
	/* An earlier load may have run out of reservation before the extent existed. */
	result = lazy_dir_reserve_extent(identifier, alloc_size);
	if (result != 0) {
		return result;
	}
	*child = identifier;
	return 0;
}

/**
 * @brief Copy a name into a directory's name arena, growing it when needed.
 *
 * @param[in,out] dir    Directory receiving the name.
 * @param[in]     name   NUL-terminated name.
 * @param[out]    offset Receives the offset of the copy.
 * @return 0 on success, or -ENOSPC when the arena cannot grow.
 */
int lazy_dir_name_copy(struct proxy_lazy_dir *dir, const char *name, uint32_t *offset)
{
	size_t length = strlen(name) + 1U;
	uint32_t capacity;

	if (dir->name_used + length <= dir->name_capacity) {
		*offset = dir->name_used;
		memcpy(dir->names + dir->name_used, name, length);
		dir->name_used += (uint32_t)length;
		return 0;
	}
	capacity = dir->name_capacity == 0U ? 512U : dir->name_capacity * 2U;
	while (capacity < dir->name_used + length) {
		if (capacity > UINT32_MAX / 2U) {
			return -ENOSPC;
		}
		capacity *= 2U;
	}
	if (capacity > CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES) {
		return -ENOSPC;
	}
	{
		char *names = lazy_alloc(capacity);

		if (names == NULL) {
			return -ENOMEM;
		}
		if (dir->names != NULL) {
			memcpy(names, dir->names, dir->name_used);
			lazy_free(dir->names);
		}
		dir->names = names;
		if (dir->cache_ready) {
			uint32_t growth = capacity - dir->name_capacity;

			lazy.cache_bytes += growth;
			dir->cache_bytes += growth;
		}
		dir->name_capacity = capacity;
	}
	*offset = dir->name_used;
	memcpy(dir->names + dir->name_used, name, length);
	dir->name_used += (uint32_t)length;
	return 0;
}

/**
 * @brief Make room for one more cached entry in a directory.
 *
 * @param[in,out] dir Directory that needs space.
 * @return 0 on success, -ENOSPC when the global cache is full, or -ENOMEM.
 */
int lazy_dir_reserve_entry(struct proxy_lazy_dir *dir)
{
	uint32_t capacity;
	int result;

	if (dir->entry_count < dir->entry_capacity) {
		return 0;
	}
	capacity = dir->entry_capacity == 0U ? 64U : dir->entry_capacity * 2U;
	if (capacity > lazy.cache_entries_max) {
		capacity = lazy.cache_entries_max;
	}
	if (capacity <= dir->entry_capacity) {
		return -ENOSPC;
	}
	result = lazy_cache_make_room((capacity - dir->entry_capacity) *
		(uint32_t)sizeof(struct proxy_lazy_dirent), capacity - dir->entry_capacity);
	if (result != 0) {
		return result;
	}
	{
		struct proxy_lazy_dirent *entries = lazy_calloc(capacity, sizeof(*entries));

		if (entries == NULL) {
			return -ENOMEM;
		}
		if (dir->entries != NULL) {
			memcpy(entries, dir->entries,
				(size_t)dir->entry_count * sizeof(*entries));
			lazy_free(dir->entries);
		}
		dir->entries = entries;
		if (dir->cache_ready) {
			uint32_t growth = (capacity - dir->entry_capacity) *
				(uint32_t)sizeof(struct proxy_lazy_dirent);

			lazy.cache_bytes += growth;
			dir->cache_bytes += growth;
		}
		dir->entry_capacity = capacity;
	}
	return 0;
}

/**
 * @brief Keep a cached directory sorted by slot.
 *
 * @param[in,out] dir Directory to sort.
 */
void lazy_dir_sort(struct proxy_lazy_dir *dir)
{
	uint32_t *order;

	if (dir->entry_count < 2U) {
		return;
	}
	order = lazy_alloc((size_t)dir->entry_count * sizeof(*order));
	if (order == NULL) {
		/* A failed sort leaves the array in insertion order, which is still usable. */
		return;
	}
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		order[index] = index;
	}
	lazy_sort_entries = dir->entries;
	qsort(order, dir->entry_count, sizeof(*order), lazy_slot_compare);
	lazy_sort_entries = NULL;
	{
		struct proxy_lazy_dirent *sorted = lazy_alloc((size_t)dir->entry_count *
			sizeof(*sorted));

		if (sorted == NULL) {
			lazy_free(order);
			return;
		}
		for (uint32_t index = 0U; index < dir->entry_count; index++) {
			sorted[index] = dir->entries[order[index]];
		}
		memcpy(dir->entries, sorted, (size_t)dir->entry_count * sizeof(*sorted));
		lazy_free(sorted);
	}
	lazy_free(order);
}

/**
 * @brief Find a cached entry by name.
 *
 * @param[in] dir            Directory to search.
 * @param[in] name           Name to find.
 * @param[in] include_deleted Whether a deleted entry may match.
 * @return The entry index, or UINT32_MAX.
 */
uint32_t lazy_dir_find_name(const struct proxy_lazy_dir *dir, const char *name,
	bool include_deleted)
{
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if ((!item->deleted || include_deleted) &&
		    strcasecmp(dir->names + item->name_offset, name) == 0) {
			return index;
		}
	}
	return UINT32_MAX;
}

/**
 * @brief Find a cached entry by one cluster of its extent.
 *
 * @details Directories use the same lookup as files, which is what turns a
 * clear-and-rewrite of a directory group at another slot into a directory
 * rename.
 *
 * @param[in] dir     Directory to search.
 * @param[in] cluster Cluster to find.
 * @param[in] include_deleted Whether a deleted entry may match.
 * @return The entry index, or UINT32_MAX.
 */
uint32_t lazy_dir_find_cluster(const struct proxy_lazy_dir *dir, uint32_t cluster,
	bool include_deleted)
{
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if ((!item->deleted || include_deleted) &&
		    item->clusters != 0U && cluster >= item->first_cluster &&
		    cluster < item->first_cluster + item->clusters) {
			return index;
		}
	}
	return UINT32_MAX;
}

/**
 * @brief Remove a deleted entry whose slots collide with a new group.
 *
 * @param[in,out] dir      Directory to change.
 * @param[in]     slot     First slot of the new group.
 * @param[in]     entries  Number of slots of the new group.
 */
void lazy_dir_clear_deleted(struct proxy_lazy_dir *dir, uint32_t slot, uint32_t entries)
{
	uint32_t kept = 0U;

	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		struct proxy_lazy_dirent *item = &dir->entries[index];

		if (item->deleted && item->slot < slot + entries &&
		    slot < item->slot + item->entries) {
			if (lazy.cache_entries != 0U) {
				lazy.cache_entries--;
			}
			continue;
		}
		if (kept != index) {
			dir->entries[kept] = *item;
		}
		kept++;
	}
	dir->entry_count = kept;
}

/**
 * @brief Insert a new cached entry at the host's slot.
 *
 * @param[in,out] dir          Directory receiving the entry.
 * @param[in]     name         Entry name.
 * @param[in]     directory    True for a directory.
 * @param[in]     slot         Absolute first slot.
 * @param[in]     first_cluster First cluster of the extent, or zero.
 * @param[in]     clusters     Number of clusters of the extent.
 * @param[in]     size         File size or directory data length.
 * @param[in]     host_owned   True when the extent comes from the host.
 * @param[in]     fat_chain    True when the host extent is a FAT chain.
 * @param[out]    index        Receives the entry index.
 * @return 0 on success, or a negative errno.
 */
int lazy_dir_insert(struct proxy_lazy_dir *dir, const char *name, bool directory,
	uint32_t slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool host_owned, bool fat_chain, uint32_t *index)
{
	struct proxy_lazy_dirent *item;
	uint32_t name_offset;
	uint32_t position;
	int result = lazy_dir_reserve_entry(dir);

	if (result != 0) {
		return result;
	}
	result = lazy_dir_name_copy(dir, name, &name_offset);
	if (result != 0) {
		return result;
	}
	lazy_dir_clear_deleted(dir, slot, lazy_name_entries(name));
	position = dir->entry_count;
	for (uint32_t cursor = 0U; cursor < dir->entry_count; cursor++) {
		if (dir->entries[cursor].slot > slot) {
			position = cursor;
			break;
		}
	}
	memmove(&dir->entries[position + 1U], &dir->entries[position],
		(size_t)(dir->entry_count - position) * sizeof(*dir->entries));
	item = &dir->entries[position];
	memset(item, 0, sizeof(*item));
	item->name_offset = name_offset;
	item->slot = slot;
	item->entries = (uint16_t)lazy_name_entries(name);
	item->directory = directory;
	item->first_cluster = first_cluster;
	item->clusters = clusters;
	item->size = size;
	item->host_owned = host_owned;
	item->fat_chain = fat_chain;
	dir->entry_count++;
	lazy.cache_entries++;
	*index = position;
	return 0;
}

/**
 * @brief Add the ranges of one host extent, walking its FAT chain when needed.
 *
 * @param[in] dir_id   Directory that owns the file.
 * @param[in] slot     Absolute slot of the entry.
 * @param[in] first    First cluster of the extent.
 * @param[in] expected Number of clusters the size asks for.
 * @param[in] fat_chain True when the host used a FAT chain.
 * @return 0 on success, or a negative errno.
 */
int lazy_add_file_ranges(uint32_t dir_id, uint32_t slot, uint32_t first,
	uint32_t expected, bool fat_chain)
{
	uint32_t cluster = first;
	uint32_t offset = 0U;
	int result;

	if (expected == 0U) {
		return 0;
	}
	if (first < PROXY_LAZY_ROOT_CLUSTER_MIN ||
	    (uint64_t)first + expected > (uint64_t)lazy.total_clusters + 2U) {
		return -EIO;
	}
	if (!fat_chain) {
		return lazy_range_add(first, expected, dir_id, PROXY_LAZY_RANGE_DATA, slot, 0U);
	}
	while (offset < expected) {
		uint32_t run = 1U;

		while (offset + run < expected) {
			uint32_t next = lazy_fat_next(cluster + run - 1U);

			if (next != cluster + run) {
				break;
			}
			run++;
		}
		if (cluster < PROXY_LAZY_ROOT_CLUSTER_MIN ||
		    (uint64_t)cluster + run > (uint64_t)lazy.total_clusters + 2U) {
			return -EIO;
		}
		result = lazy_range_add(cluster, run, dir_id, PROXY_LAZY_RANGE_DATA, slot, offset);
		if (result != 0) {
			return result;
		}
		offset += run;
		if (offset < expected) {
			cluster = lazy_fat_next(cluster + run - 1U);
			if (cluster < PROXY_LAZY_ROOT_CLUSTER_MIN ||
			    cluster >= lazy.total_clusters + 2U) {
				return -EIO;
			}
		}
	}
	return 0;
}

/**
 * @brief Rebuild every data range of a directory from its cached entries.
 *
 * @param[in] dir_id Directory identifier.
 * @return 0 on success, or a negative errno.
 */
int lazy_rebuild_data_ranges(uint32_t dir_id)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	int result = 0;

	lazy_range_remove_owner(dir_id, PROXY_LAZY_RANGE_DATA);
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if (item->directory || item->deleted || item->clusters == 0U) {
			continue;
		}
		result = lazy_add_file_ranges(dir_id, item->slot, item->first_cluster,
			item->clusters, item->fat_chain);
		if (result != 0) {
			lazy.last_error = result;
			return result;
		}
	}
	dir->ranges_dirty = false;
	return 0;
}

/**
 * @brief Return the next slot that no entry and no overlay record occupies.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] dir    Directory being built.
 * @param[in] count  Number of entries added so far.
 * @param[in] group  Slots the candidate entry group occupies.
 * @param[in] slot   Candidate slot.
 * @return A free absolute slot.
 */
uint32_t lazy_free_slot(uint32_t dir_id, const struct proxy_lazy_dir *dir,
	uint32_t count, uint32_t group, uint32_t slot)
{
	for (;;) {
		bool blocked = false;

		for (uint32_t index = 0U; index < count; index++) {
			if (slot < dir->entries[index].slot + dir->entries[index].entries &&
			    dir->entries[index].slot < slot + group) {
				blocked = true;
				break;
			}
		}
		for (uint32_t index = 0U; !blocked &&
		     index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
			const struct proxy_lazy_overlay *record = &lazy.overlay[index];

			if (record->used && record->dir_id == dir_id) {
				if (slot < record->slot + record->entries &&
				    record->slot < slot + group) {
					blocked = true;
				}
			}
		}
		if (!blocked) {
			return slot;
		}
		slot += 2U;
	}
}
