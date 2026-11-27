// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_dir.c
 * @brief Persistent directory headers, paths, child lists, counting and extent reservation.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- directory paths ---- */

/**
 * @brief Rebuild the source path of a view directory.
 *
 * @details The path is the mount point of the owning partition followed by the
 * names of the directories between it and @p identifier. The chain is walked
 * upwards and written backwards, so nothing per-directory path is stored.
 *
 * @param[in]  identifier Directory identifier.
 * @param[out] path       Buffer receiving the path.
 * @param[in]  size       Size of @p path in bytes.
 * @return 0 on success, or a negative errno.
 */
int lazy_dir_path(uint32_t identifier, char *path, size_t size)
{
	uint32_t chain[PROXY_LAZY_MAX_DEPTH];
	uint32_t depth = 0U;
	uint32_t current = identifier;
	uint32_t partition = UINT32_MAX;
	int written;

	if (identifier >= lazy.dir_count) {
		return -ENOENT;
	}
	while (current != PROXY_LAZY_ROOT_ID) {
		struct proxy_lazy_dir *dir = &lazy.dirs[current];

		if (dir->is_partition_root) {
			partition = dir->partition;
			break;
		}
		if (depth >= PROXY_LAZY_MAX_DEPTH || dir->parent == PROXY_LAZY_INVALID_DIR ||
		    dir->parent == PROXY_LAZY_ROOT_ID) {
			return -ENOENT;
		}
		chain[depth++] = dir->name_offset;
		current = dir->parent;
	}
	if (partition >= PROXY_LAZY_PARTITIONS || lazy.partitions[partition].mount[0] == '\0') {
		return -ENOENT;
	}
	written = snprintf(path, size, "%s", lazy.partitions[partition].mount);
	if (written < 0 || (size_t)written >= size) {
		return -ENAMETOOLONG;
	}
	while (depth != 0U) {
		const char *name = lazy.name_arena + chain[--depth];
		int added = snprintf(path + written, size - (size_t)written, "/%s", name);

		if (added < 0 || (size_t)added >= size - (size_t)written) {
			return -ENAMETOOLONG;
		}
		written += added;
	}
	return 0;
}

/**
 * @brief Rebuild the source path of one cached directory entry.
 *
 * @param[in]  dir_id Directory identifier.
 * @param[in]  item   Cached entry.
 * @param[out] path   Buffer receiving the path.
 * @param[in]  size   Size of @p path in bytes.
 * @return 0 on success, or a negative errno.
 */
int lazy_entry_path(uint32_t dir_id, const struct proxy_lazy_dirent *item,
	char *path, size_t size)
{
	size_t length;
	int result = lazy_dir_path(dir_id, path, size);
	int added;

	if (result != 0) {
		return result;
	}
	length = strlen(path);
	added = snprintf(path + length, size - length, "/%s", lazy.dirs[dir_id].names + item->name_offset);
	if (added < 0 || (size_t)added >= size - length) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/* ---- directory headers ---- */

/**
 * @brief Allocate one persistent directory header.
 *
 * @param[in] name Directory name.
 * @return The new identifier, or UINT32_MAX when the table or arena is full.
 */
uint32_t lazy_dir_alloc(const char *name)
{
	uint32_t identifier;
	uint32_t name_offset;

	if (lazy.dir_count >= lazy.dir_capacity) {
		lazy.truncated = true;
		return UINT32_MAX;
	}
	name_offset = lazy_store_name(name);
	if (name_offset == UINT32_MAX) {
		lazy.truncated = true;
		return UINT32_MAX;
	}
	identifier = lazy.dir_count++;
	memset(&lazy.dirs[identifier], 0, sizeof(lazy.dirs[identifier]));
	lazy.dirs[identifier].parent = PROXY_LAZY_INVALID_DIR;
	lazy.dirs[identifier].partition = PROXY_LAZY_PARTITIONS;
	lazy.dirs[identifier].name_offset = name_offset;
	lazy.dirs[identifier].lru_prev = PROXY_LAZY_INVALID_DIR;
	lazy.dirs[identifier].lru_next = PROXY_LAZY_INVALID_DIR;
	return identifier;
}

/**
 * @brief Grow the child-directory list of a header.
 *
 * @param[in,out] dir Directory whose list grows.
 * @return 0 on success, or -ENOMEM.
 */
int lazy_subdir_reserve(struct proxy_lazy_dir *dir)
{
	uint32_t capacity;
	uint32_t *subdirs;

	if (dir->subdir_count < dir->subdir_capacity) {
		return 0;
	}
	capacity = dir->subdir_capacity == 0U ? 8U : dir->subdir_capacity * 2U;
	if (capacity > CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS) {
		capacity = CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS;
	}
	if (capacity <= dir->subdir_capacity) {
		return -ENOMEM;
	}
	subdirs = lazy_alloc((size_t)capacity * sizeof(*subdirs));
	if (subdirs == NULL) {
		return -ENOMEM;
	}
	if (dir->subdirs != NULL) {
		memcpy(subdirs, dir->subdirs, (size_t)dir->subdir_count * sizeof(*subdirs));
		lazy_free(dir->subdirs);
	}
	dir->subdirs = subdirs;
	dir->subdir_capacity = capacity;
	return 0;
}

/**
 * @brief Register an existing child header under a parent.
 *
 * @param[in,out] parent Parent directory.
 * @param[in]     child  Child identifier.
 * @return 0 on success, or -ENOMEM.
 */
int lazy_subdir_append(struct proxy_lazy_dir *parent, uint32_t child)
{
	int result = lazy_subdir_reserve(parent);

	if (result != 0) {
		return result;
	}
	parent->subdirs[parent->subdir_count++] = child;
	return 0;
}

/* ---- counting and reserving a directory extent ---- */

/**
 * @brief Count the entries and slots of a source directory.
 *
 * @details One opendir/readdir pass gathers only the slot count and the name
 * bytes that a later indexing pass will need. The walk stops at the configured
 * entry cap and marks the directory truncated.
 *
 * @param[in,out] dir        Directory being measured.
 * @param[out]    entries    Receives the number of entries found.
 * @param[out]    slots      Receives the number of slots, including the terminator.
 * @param[out]    name_bytes Receives the bytes the names need.
 * @return 0 on success, or a negative errno from the source directory.
 */
int lazy_dir_count(struct proxy_lazy_dir *dir, uint32_t *entries, uint32_t *slots,
	uint32_t *name_bytes)
{
	char path[VFS_PATH_MAX];
	struct vfs_dirent *entry;
	struct vfs_dir *handle;
	uint32_t found = 0U;
	uint32_t found_slots = 0U;
	uint64_t found_names = 0U;
	int error = 0;
	int result;

	*entries = 0U;
	*slots = 0U;
	*name_bytes = 0U;
	result = lazy_dir_path((uint32_t)(dir - lazy.dirs), path, sizeof(path));
	if (result != 0) {
		return result;
	}
	handle = vfs_opendir(path, &error);
	if (handle == NULL) {
		return error != 0 ? error : -EIO;
	}
	entry = lazy_alloc(sizeof(*entry));
	if (entry == NULL) {
		vfs_closedir(handle);
		return -ENOMEM;
	}
	if (lazy_cancel_requested()) {
		vfs_closedir(handle);
		lazy_free(entry);
		return -ECANCELED;
	}
	result = vfs_readdir(handle, entry);
	while (result == 0 && found < lazy.dir_max_entries) {
		size_t name_length = strlen(entry->name) + 1U;

		found++;
		found_slots += lazy_name_entries(entry->name);
		found_names += name_length;
		if (found % PROXY_LAZY_PROGRESS_INTERVAL == 0U) {
			lazy_progress_report(found, path);
			if (lazy_cancel_requested()) {
				result = -ECANCELED;
				break;
			}
		}
		result = vfs_readdir(handle, entry);
	}
	if (result == 0) {
		dir->truncated = true;
		lazy.truncated = true;
	} else if (result != -ENOENT) {
		vfs_closedir(handle);
		lazy_free(entry);
		return result;
	}
	vfs_closedir(handle);
	lazy_free(entry);
	found_slots += 1U;
	*entries = found;
	*slots = found_slots;
	*name_bytes = found_names > UINT32_MAX ? UINT32_MAX : (uint32_t)found_names;
	return 0;
}

/**
 * @brief Reserve the directory extent of a view directory.
 *
 * @details The extent is sized from the actual entry count of the source
 * directory, so a directory with more than one cluster of entries is rendered
 * correctly. A source directory that cannot be opened still gets one cluster,
 * so the view stays readable; its folder simply renders as empty.
 *
 * @param[in] identifier Directory identifier.
 * @return 0 on success, or -ENOSPC or -ENOMEM.
 */
int lazy_dir_reserve_extent(uint32_t identifier, uint64_t alloc_size)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[identifier];
	struct proxy_lazy_partition *part;
	uint32_t entries = 0U;
	uint32_t slots = 1U;
	uint32_t names = 0U;
	uint32_t clusters;
	int result;

	if (dir->is_root) {
		dir->extent_ready = true;
		return 0;
	}
	if (!dir->extent_ready) {
		part = &lazy.partitions[dir->partition];
		if (alloc_size != 0U && part->selected && part->mounted) {
			/*
			 * ext4 reports the directory data length. An ext4 dirent is at
			 * least 12 bytes, so alloc_size / 4 is an upper bound on the
			 * exFAT slots (3 per short entry); sizing from it avoids a
			 * count-only readdir of every child when the parent is opened.
			 */
			uint64_t raw_slots = alloc_size / 4U;
			uint64_t raw_entries = raw_slots / 3U;

			if (raw_slots == 0U) {
				raw_slots = 1U;
			}
			if (raw_entries == 0U) {
				raw_entries = 1U;
			}
			if (raw_entries > lazy.dir_max_entries) {
				raw_entries = lazy.dir_max_entries;
			}
			if (raw_entries > lazy.cache_entries_max) {
				raw_entries = lazy.cache_entries_max;
			}
			entries = (uint32_t)raw_entries;
			slots = entries * 3U;
			if (slots == 0U) {
				slots = 1U;
			}
			names = slots > UINT32_MAX / 16U ? UINT32_MAX : slots * 16U;
		} else if (part->selected && part->mounted) {
			result = lazy_dir_count(dir, &entries, &slots, &names);
			if (result == -ECANCELED) {
				/* Leave the extent unreserved so a later read starts clean. */
				lazy.last_error = result;
				return result;
			}
			if (result != 0) {
				pr_warn("lazy proxy: cannot index %s: %d\n",
					lazy_dir_name(identifier), result);
				dir->source_error = result;
				lazy.last_error = result;
				entries = 0U;
				slots = 1U;
				names = 0U;
			}
		} else {
			dir->source_error = -ENODEV;
		}
		dir->planned_entries = entries;
		dir->planned_slots = slots;
		dir->planned_name_bytes = names;
		clusters = (slots + PROXY_LAZY_SLOTS_PER_CLUSTER - 1U) /
			PROXY_LAZY_SLOTS_PER_CLUSTER;
		if (clusters == 0U) {
			clusters = 1U;
		}
		/* Leave a little room so the host can create entries without a directory extension. */
		clusters += CONFIG_STORAGE_PROXY_LAZY_DIR_SLACK_CLUSTERS;
		result = lazy_alloc_clusters(dir->partition, clusters, &dir->first_cluster,
			&dir->clusters);
		if (result != 0) {
			lazy.last_error = result;
			return result;
		}
		dir->extension_cluster = dir->first_cluster + dir->clusters - 1U;
		dir->extent_ready = true;
	}
	if (!dir->range_ready) {
		result = lazy_range_add(dir->first_cluster, dir->clusters, identifier,
			PROXY_LAZY_RANGE_DIRECTORY, PROXY_LAZY_INVALID_SLOT, 0U);
		if (result != 0) {
			lazy.last_error = result;
			return result;
		}
		dir->range_ready = true;
	}
	return 0;
}
