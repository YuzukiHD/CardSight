// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_write_ns.c
 * @brief Host create, rename, delete, resize and synchronisation.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Build the source path of a child of a directory.
 *
 * @param[in]  dir_id Directory identifier.
 * @param[in]  name   Child name.
 * @param[out] path   Buffer receiving the path.
 * @param[in]  size   Size of @p path.
 * @return 0 on success, or a negative errno.
 */
int lazy_child_path(uint32_t dir_id, const char *name, char *path, size_t size)
{
	size_t length;
	int written;

	if (lazy_dir_path(dir_id, path, size) != 0) {
		return -ENAMETOOLONG;
	}
	length = strlen(path);
	written = snprintf(path + length, size - length, "/%s", name);
	if (written < 0 || (size_t)written >= size - length) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/**
 * @brief Create a source object the host created.
 *
 * @details A source name that still exists because an earlier delete was not
 * flushed yet is handled the way the eager engine does it: the deferred deletes
 * are carried out and the create is retried once.
 *
 * @param[in] dir_id    Parent directory.
 * @param[in] name      Entry name.
 * @param[in] directory True for a directory.
 * @return 0 on success, or a negative errno.
 */
int lazy_source_create(uint32_t dir_id, const char *name, bool directory)
{
	char path[VFS_PATH_MAX];
	int result = lazy_child_path(dir_id, name, path, sizeof(path));

	for (uint32_t attempt = 0U; result == 0 && attempt < 2U; attempt++) {
		if (directory) {
			result = vfs_mkdir(path);
		} else {
			int descriptor = vfs_open(path,
				VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL);

			result = descriptor < 0 ? descriptor : vfs_close(descriptor);
		}
		if (result != -EEXIST || attempt != 0U) {
			break;
		}
		result = lazy_apply_deletes_locked();
	}
	return result;
}

/**
 * @brief Truncate the source file of a cached entry.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index.
 * @param[in] size        New size.
 * @return 0 on success, or a negative errno.
 */
int lazy_truncate_entry(uint32_t dir_id, uint32_t entry_index, uint64_t size)
{
	const struct proxy_lazy_dirent *item = &lazy.dirs[dir_id].entries[entry_index];
	char path[VFS_PATH_MAX];
	int descriptor;
	int result;

	if (item->directory) {
		return 0;
	}
	result = lazy_entry_path(dir_id, item, path, sizeof(path));
	if (result != 0) {
		return result;
	}
	if (lazy.write_fd >= 0 && lazy.write_dir == dir_id &&
	    lazy.write_slot == item->slot) {
		lazy_write_cache_drop();
	}
	if (lazy.cached_fd >= 0 && lazy.cached_dir == dir_id &&
	    lazy.cached_slot == item->slot) {
		(void)vfs_close(lazy.cached_fd);
		lazy.cached_fd = -1;
		lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	}
	descriptor = vfs_open(path, VFS_O_WRITE);
	if (descriptor < 0) {
		return descriptor == -ENOENT ? 0 : descriptor;
	}
	result = vfs_ftruncate(descriptor, size);
	(void)vfs_close(descriptor);
	return result;
}

/**
 * @brief Send the ordinary entry fields of a host write to the source.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index.
 * @param[in] size        New file size or directory data length.
 * @return 0 on success, or a negative errno.
 */
int lazy_entry_resize(uint32_t dir_id, uint32_t entry_index, uint64_t size)
{
	struct proxy_lazy_dirent *item = &lazy.dirs[dir_id].entries[entry_index];
	uint64_t old_size = item->size;

	item->size = size;
	if (!item->directory && old_size != size) {
		return lazy_truncate_entry(dir_id, entry_index, size);
	}
	return 0;
}

/**
 * @brief Mark the cached entry of a directory as a FAT chain.
 *
 * @details The parent entry advertises the child's extent; once the child is
 * fragmented, that entry must clear exFAT's NoFatChain flag so the host follows
 * the FAT instead of assuming contiguity.
 *
 * @param[in] dir_id Directory whose extent became a chain.
 */
void lazy_dir_parent_chained(uint32_t dir_id)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dir *parent;
	uint32_t index;

	if (dir->parent >= lazy.dir_count) {
		return;
	}
	parent = &lazy.dirs[dir->parent];
	if (!parent->cache_ready || parent->entries == NULL) {
		return;
	}
	index = lazy_dir_find_name(parent, lazy_dir_name(dir_id), false);
	if (index != UINT32_MAX) {
		parent->entries[index].fat_chain = true;
	}
}

/**
 * @brief Try to grow a directory extent within the reservation.
 *
 * @details exFAT directories are contiguous unless their chain is fragmented.
 * Clusters that follow the extent and belong to no other object can simply
 * join it; anything else is refused rather than misrepresented.
 *
 * @param[in] dir_id  Directory identifier.
 * @param[in] wanted  Clusters the directory data length asks for.
 * @return 0 on success, or -ENOSPC when the extension cannot be represented.
 */
int lazy_dir_extend(uint32_t dir_id, uint32_t wanted)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];

	if (wanted <= dir->clusters) {
		return 0;
	}
	if (dir->truncated) {
		/*
		 * The directory hid entries, so growing it would let the host write into
		 * slots this engine never advertised. Refuse rather than corrupt.
		 */
		lazy.last_error = -ENOSPC;
		return -ENOSPC;
	}
	while (dir->clusters < wanted) {
		struct proxy_lazy_partition *part = &lazy.partitions[dir->partition];
		uint32_t contiguous = dir->first_cluster + dir->clusters;
		uint32_t next;

		if (contiguous == part->bump &&
		    (uint64_t)part->bump + 1U <=
		    (uint64_t)part->reserve_first + part->reserve_clusters) {
			part->bump++;
			next = contiguous;
		} else {
			struct proxy_lazy_range occupied;

			if (contiguous < lazy.total_clusters + 2U &&
			    lazy_range_find(contiguous, &occupied) != 0 &&
			    (lazy.bitmap[(contiguous - 2U) / 8U] &
			     (uint8_t)(1U << ((contiguous - 2U) % 8U))) != 0U) {
				/*
				 * The parent entry reports a longer directory and the
				 * host marked the following cluster used: a contiguous
				 * extension, with or without a FAT link.
				 */
				next = contiguous;
			} else {
				/* Otherwise the host may have linked a new cluster on the FAT. */
				next = lazy_fat_next(dir->extension_cluster);
				if (next < PROXY_LAZY_ROOT_CLUSTER_MIN ||
				    next >= lazy.total_clusters + 2U ||
				    lazy_range_find(next, &occupied) == 0) {
					lazy.last_error = -ENOSPC;
					return -ENOSPC;
				}
			}
		}
		lazy_pending_drop_cluster(next);
		if (next != contiguous) {
			dir->chained = true;
			lazy_dir_parent_chained(dir_id);
		}
		if (lazy_range_add(next, 1U, dir_id, PROXY_LAZY_RANGE_DIRECTORY,
		    PROXY_LAZY_INVALID_SLOT, dir->clusters) != 0) {
			return -ENOMEM;
		}
		dir->clusters++;
		dir->extension_cluster = next;
	}
	return 0;
}

/**
 * @brief Apply a host entry that names an entry the cache already has.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index.
 * @param[in] directory   True for a directory.
 * @param[in] host_slot   Absolute first slot.
 * @param[in] first_cluster First cluster the host entry names.
 * @param[in] clusters    Clusters the host size asks for.
 * @param[in] size        File size or directory data length.
 * @param[in] fat_chain   True when the host extent is a FAT chain.
 * @return 0 on success, or a negative errno.
 */
int lazy_update_entry(uint32_t dir_id, uint32_t entry_index, bool directory,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dirent *item = &dir->entries[entry_index];
	int result = 0;

	if (item->directory != directory) {
		return -EIO;
	}
	item->slot = host_slot;
	if (directory) {
		uint32_t child = lazy_dir_find_child(dir, dir->names + item->name_offset);

		if (child == PROXY_LAZY_INVALID_DIR) {
			return -EIO;
		}
		result = lazy_dir_extend(child, clusters != 0U ? clusters : 1U);
		if (result != 0) {
			return result;
		}
		item->first_cluster = lazy.dirs[child].first_cluster;
		item->clusters = lazy.dirs[child].clusters;
		item->fat_chain = lazy.dirs[child].chained;
	} else if (size == 0U) {
		item->first_cluster = 0U;
		item->clusters = 0U;
		item->fat_chain = false;
	} else if (first_cluster < PROXY_LAZY_ROOT_CLUSTER_MIN) {
		return -EIO;
	} else if (first_cluster != item->first_cluster || fat_chain) {
		/* The host moved the data or linked it through the FAT. */
		item->first_cluster = first_cluster;
		item->clusters = clusters;
		item->host_owned = true;
		item->fat_chain = fat_chain;
	} else if (!item->host_owned && clusters > item->clusters) {
		/*
		 * A synthetic extent cannot grow into the next file's clusters. The
		 * source entry asks for more than the view advertised, which means the
		 * host wrote somewhere this engine did not map.
		 */
		return -ENOSPC;
	} else {
		item->clusters = clusters;
	}
	result = lazy_entry_resize(dir_id, entry_index, size);
	if (result != 0) {
		return result;
	}
	dir->ranges_dirty = true;
	return 0;
}

/**
 * @brief Remove a source object again after a half-applied host create.
 *
 * @param[in] dir_id    Parent directory.
 * @param[in] name      Entry name.
 * @param[in] directory True for a directory.
 */
void lazy_source_remove(uint32_t dir_id, const char *name, bool directory)
{
	char path[VFS_PATH_MAX];

	if (lazy_child_path(dir_id, name, path, sizeof(path)) != 0) {
		return;
	}
	if (directory) {
		(void)vfs_rmdir(path);
	} else {
		(void)vfs_unlink(path);
	}
}

/**
 * @brief Adopt a host extent for a host-created file or directory.
 *
 * @param[in] dir_id      Parent directory identifier.
 * @param[in] name        Entry name.
 * @param[in] directory   True for a directory.
 * @param[in] host_slot   Absolute first slot.
 * @param[in] first_cluster First cluster the host entry names.
 * @param[in] clusters    Clusters the size asks for.
 * @param[in] size        File size or directory data length.
 * @param[in] fat_chain   True when the host extent is a FAT chain.
 * @return 0 on success, or a negative errno.
 */
int lazy_create_entry(uint32_t dir_id, const char *name, bool directory,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint32_t extent_first = first_cluster;
	uint32_t extent_clusters = clusters;
	uint32_t inserted;
	int result = lazy_source_create(dir_id, name, directory);

	if (result != 0) {
		return result;
	}
	if (directory) {
		uint32_t child;
		struct proxy_lazy_dir *header;

		if (extent_clusters == 0U) {
			extent_clusters = 1U;
		}
		if (extent_first < PROXY_LAZY_ROOT_CLUSTER_MIN) {
			result = lazy_alloc_clusters(dir->partition, extent_clusters,
				&extent_first, &extent_clusters);
			if (result != 0) {
				lazy_source_remove(dir_id, name, true);
				return result;
			}
		}
		child = lazy_dir_find_child(dir, name);
		if (child == PROXY_LAZY_INVALID_DIR) {
			result = lazy_subdir_reserve(dir);
			if (result != 0) {
				lazy_source_remove(dir_id, name, true);
				return result;
			}
			child = lazy_dir_alloc(name);
			if (child == UINT32_MAX) {
				lazy_source_remove(dir_id, name, true);
				return -ENOSPC;
			}
			lazy.dirs[child].parent = dir_id;
			lazy.dirs[child].partition = dir->partition;
			result = lazy_subdir_append(dir, child);
			if (result != 0) {
				lazy_source_remove(dir_id, name, true);
				return result;
			}
		}
		header = &lazy.dirs[child];
		if (header->range_ready) {
			lazy_range_remove_owner(child, PROXY_LAZY_RANGE_DIRECTORY);
			lazy_cache_evict(child);
		}
		lazy_overlay_clear_dir(child);
		header->first_cluster = extent_first;
		header->clusters = extent_clusters;
		header->extension_cluster = extent_first + extent_clusters - 1U;
		header->extent_ready = true;
		header->range_ready = false;
		header->chained = false;
		result = lazy_range_add(extent_first, extent_clusters, child,
			PROXY_LAZY_RANGE_DIRECTORY, PROXY_LAZY_INVALID_SLOT, 0U);
		if (result != 0) {
			lazy_source_remove(dir_id, name, true);
			return result;
		}
		header->range_ready = true;
		/* Zeroing a fresh host directory is synthesized, not file data. */
		for (uint32_t cluster = extent_first;
		     cluster < extent_first + extent_clusters; cluster++) {
			lazy_pending_drop_cluster(cluster);
			lazy_run_drop_cluster(cluster);
		}
	} else if (size != 0U && extent_first < PROXY_LAZY_ROOT_CLUSTER_MIN) {
		lazy_source_remove(dir_id, name, false);
		return -EIO;
	}
	result = lazy_dir_insert(dir, name, directory, host_slot, extent_first,
		extent_clusters, size, true, directory ? false : fat_chain, &inserted);
	if (result != 0) {
		lazy_source_remove(dir_id, name, directory);
		return result;
	}
	dir->ranges_dirty = true;
	return 0;
}

/**
 * @brief Forget the overlay record of one name.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] name   Entry name.
 */
void lazy_overlay_forget(uint32_t dir_id, const char *name)
{
	struct proxy_lazy_overlay *record = lazy_overlay_find_name(dir_id, name);

	if (record != NULL && record->used) {
		record->used = false;
		if (lazy.overlay_count != 0U) {
			lazy.overlay_count--;
		}
	}
}

/**
 * @brief Apply a host entry that renames an entry the cache already has.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index of the old name.
 * @param[in] name        New name.
 * @param[in] host_slot   New absolute first slot.
 * @param[in] first_cluster First cluster of the host extent.
 * @param[in] clusters    Clusters the size asks for.
 * @param[in] size        File size or directory data length.
 * @param[in] fat_chain   True when the host extent is a FAT chain.
 * @return 0 on success, or a negative errno.
 */
int lazy_rename_entry(uint32_t dir_id, uint32_t entry_index, const char *name,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dirent *item = &dir->entries[entry_index];
	char old_name[VFS_NAME_MAX + 1];
	char old_path[VFS_PATH_MAX];
	char new_path[VFS_PATH_MAX];
	uint32_t name_offset;
	int result;

	snprintf(old_name, sizeof(old_name), "%s", dir->names + item->name_offset);
	result = lazy_entry_path(dir_id, item, old_path, sizeof(old_path));
	if (result != 0) {
		return result;
	}
	result = lazy_child_path(dir_id, name, new_path, sizeof(new_path));
	if (result != 0) {
		return result;
	}
	result = lazy_apply_deletes_locked();
	if (result != 0) {
		return result;
	}
	result = vfs_rename(old_path, new_path);
	if (result == -EEXIST) {
		result = lazy_apply_deletes_locked();
		if (result == 0) {
			result = vfs_rename(old_path, new_path);
		}
	}
	if (result != 0) {
		return result;
	}
	if (item->directory) {
		uint32_t child = lazy_dir_find_child(dir, old_name);

		if (child != PROXY_LAZY_INVALID_DIR) {
			uint32_t offset = lazy_store_name(name);

			if (offset == UINT32_MAX) {
				return -ENOSPC;
			}
			lazy.dirs[child].name_offset = offset;
			lazy.dirs[child].parent = dir_id;
		}
	}
	lazy_overlay_forget(dir_id, old_name);
	result = lazy_dir_name_copy(dir, name, &name_offset);
	if (result != 0) {
		return result;
	}
	item->name_offset = name_offset;
	item->entries = (uint16_t)lazy_name_entries(name);
	result = lazy_update_entry(dir_id, entry_index, item->directory, host_slot,
		first_cluster, clusters, size, fat_chain);
	if (result != 0) {
		return result;
	}
	return 0;
}

/**
 * @brief Mark the entry at an absolute slot deleted and remember the tombstone.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] slot   Absolute slot of the deleted stream entry.
 * @return 0 on success, or -ENOSPC when the overlay is full.
 */
int lazy_delete_entry(uint32_t dir_id, uint32_t slot)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint32_t index = lazy_entry_by_slot(dir, slot);
	struct proxy_lazy_dirent *item;
	int result;

	if (index == UINT32_MAX || dir->entries[index].deleted) {
		return 0;
	}
	item = &dir->entries[index];
	result = lazy_overlay_tombstone(dir_id, dir->names + item->name_offset,
		item->slot, item->directory, item->entries);
	if (result != 0) {
		return result;
	}
	item->deleted = true;
	dir->ranges_dirty = true;
	return 0;
}

/**
 * @brief Apply one parsed host entry.
 *
 * @details The entry is matched by name first, then by the cluster it names,
 * which is what turns a clear-and-rewrite at another slot into a rename. An
 * unknown name creates the source object. The overlay records the result so a
 * later cache reload reproduces the host's view.
 *
 * @param[in] dir_id        Parent directory.
 * @param[in] name          Entry name.
 * @param[in] directory     True for a directory.
 * @param[in] first_cluster First cluster named by the host.
 * @param[in] size          File size or directory data length.
 * @param[in] fat_chain     True when the host extent is a FAT chain.
 * @param[in] host_slot     Absolute first slot.
 * @return 0 on success, or a negative errno.
 */
int lazy_sync_entry(uint32_t dir_id, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, bool fat_chain, uint32_t host_slot)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint32_t clusters = size == 0U ? 0U :
		(uint32_t)((size + PROXY_LAZY_CLUSTER_SIZE - 1U) / PROXY_LAZY_CLUSTER_SIZE);
	uint32_t index = lazy_dir_find_name(dir, name, false);
	int result;

	if (directory && clusters == 0U) {
		clusters = 1U;
	}
	if (index != UINT32_MAX) {
		result = lazy_update_entry(dir_id, index, directory, host_slot,
			first_cluster, clusters, size, fat_chain);
	} else {
		uint32_t old = first_cluster != 0U ?
			lazy_dir_find_cluster(dir, first_cluster, true) : UINT32_MAX;

		if (old != UINT32_MAX && dir->entries[old].directory == directory &&
		    !dir->entries[old].deleted) {
			result = lazy_rename_entry(dir_id, old, name, host_slot,
				first_cluster, clusters, size, fat_chain);
		} else if (old != UINT32_MAX && dir->entries[old].deleted &&
			   dir->entries[old].directory == directory) {
			/* Delete and re-create of the same object in one batch. */
			lazy_overlay_forget(dir_id, dir->names + dir->entries[old].name_offset);
			dir->entries[old].deleted = false;
			result = lazy_rename_entry(dir_id, old, name, host_slot,
				first_cluster, clusters, size, fat_chain);
		} else {
			result = lazy_create_entry(dir_id, name, directory, host_slot,
				first_cluster, clusters, size, fat_chain);
		}
	}
	if (result != 0) {
		pr_warn("lazy proxy: sync %s/%s slot %u size %llu failed: %d\n",
			lazy_dir_name(dir_id), name, host_slot,
			(unsigned long long)size, result);
		lazy.last_error = result;
		return result;
	}
	result = lazy_overlay_put(dir_id, name, host_slot, first_cluster, clusters, size,
		directory, fat_chain);
	if (result != 0) {
		lazy.last_error = result;
		return result;
	}
	return 0;
}
