// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_io.c
 * @brief Block-device write/flush callbacks, pending deletes and size enforcement.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Carry out the deletes the host requested.
 *
 * @details Files go first, then directories are retried until no more become
 * empty, so a recursively deleted tree is removed in one pass. A directory
 * that is still not empty stays for the next flush rather than failing the
 * host. A name that is already gone counts as deleted.
 *
 * @return 0 on success, or the first hard error.
 */
int lazy_apply_deletes_locked(void)
{
	int result = 0;
	bool progress;

	if (lazy.overlay == NULL) {
		return 0;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		struct proxy_lazy_overlay *record = &lazy.overlay[index];
		char path[VFS_PATH_MAX];
		int deleted;

		if (!record->used || !record->deleted || record->applied ||
		    record->directory) {
			continue;
		}
		if (lazy_child_path(record->dir_id, lazy.overlay_names + record->name_offset,
		    path, sizeof(path)) != 0) {
			record->used = false;
			continue;
		}
		deleted = vfs_unlink(path);
		if (deleted != 0 && deleted != -ENOENT && result == 0) {
			result = deleted;
		}
		if (deleted == 0 || deleted == -ENOENT) {
			record->used = false;
		}
	}
	do {
		progress = false;
		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
			struct proxy_lazy_overlay *record = &lazy.overlay[index];
			char path[VFS_PATH_MAX];
			int deleted;

			if (!record->used || !record->deleted || record->applied ||
			    !record->directory) {
				continue;
			}
			if (lazy_child_path(record->dir_id,
			    lazy.overlay_names + record->name_offset, path,
			    sizeof(path)) != 0) {
				record->used = false;
				continue;
			}
			deleted = vfs_rmdir(path);
			if (deleted == 0 || deleted == -ENOENT) {
				record->used = false;
				progress = true;
			} else if (deleted != -ENOTEMPTY && result == 0) {
				result = deleted;
			}
		}
	} while (progress && result == 0);
	lazy.overlay_count = 0U;
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		if (lazy.overlay[index].used) {
			lazy.overlay_count++;
		}
	}
	return result;
}

/**
 * @brief Enforce the announced size of every source file the host wrote.
 *
 * @details A host writes whole sectors, so the source file is usually longer
 * than the ValidDataLength it announced. Only directories whose file data was
 * written are visited.
 *
 * @return 0 on success, or the first error.
 */
int lazy_enforce_sizes_locked(void)
{
	int result = 0;

	for (uint32_t index = 0U; index < lazy.dir_count; index++) {
		struct proxy_lazy_dir *dir = &lazy.dirs[index];

		if (!dir->cache_ready || !dir->sizes_dirty || dir->entries == NULL) {
			continue;
		}
		for (uint32_t entry = 0U; entry < dir->entry_count; entry++) {
			const struct proxy_lazy_dirent *item = &dir->entries[entry];

			if (!item->directory && !item->deleted && item->clusters != 0U) {
				int resized = lazy_truncate_entry(index, entry, item->size);

				if (resized != 0 && result == 0) {
					result = resized;
				}
			}
		}
		dir->sizes_dirty = false;
	}
	return result;
}

/**
 * @brief Flush queued writes, deferred deletes and the source mounts.
 *
 * @details Idempotent: a successful pass sets @c clean, and a second call does
 * nothing until the host writes again.
 *
 * @return 0 on success, or a negative errno.
 */
int lazy_flush_locked(void)
{
	int result = 0;

	if (lazy.clean) {
		return 0;
	}
	result = lazy_claim_runs();
	if (result == 0) {
		result = lazy_enforce_sizes_locked();
	}
	if (result == 0) {
		result = lazy_apply_deletes_locked();
	}
	lazy_write_cache_drop();
	for (uint32_t partition = 0U; result == 0 &&
	     partition < PROXY_LAZY_PARTITIONS; partition++) {
		if (lazy.partitions[partition].mounted) {
			result = vfs_sync_mount(lazy.partitions[partition].mount);
		}
	}
	if (result == 0) {
		lazy.clean = true;
		lazy.flush_failed = false;
	} else {
		lazy.flush_failed = true;
		pr_warn("lazy proxy: flush failed: %d\n", result);
	}
	return result;
}

/**
 * @brief Write one host request straight into a file when it maps contiguously.
 *
 * @details A host sends large transfers for a file it already announced, so
 * every sector of the request maps to the same data range. One @c vfs_write of
 * the whole request replaces one 512-byte write per sector and avoids the
 * journal and block-cache overhead that made a large copy crawl.
 *
 * @param[in]  sector First logical sector.
 * @param[in]  input  Sector contents.
 * @param[in]  count  Sectors available.
 * @param[out] result Receives the write error when sectors were consumed.
 * @return Sectors consumed; zero when the request needs the sector path.
 */
uint32_t lazy_write_data_run(uint64_t sector, const uint8_t *input, uint32_t count,
	int *result)
{
	struct proxy_lazy_range range;
	uint64_t relative;
	uint32_t cluster;
	uint32_t sector_in_cluster;
	uint32_t entry_index;
	uint64_t file_offset;
	uint64_t last_cluster;
	uint64_t length;

	if (sector < lazy.data_start || count == 0U) {
		return 0U;
	}
	relative = sector - lazy.data_start;
	cluster = (uint32_t)(relative / PROXY_LAZY_SECTORS_PER_CLUSTER) +
		PROXY_LAZY_ROOT_CLUSTER_MIN;
	sector_in_cluster = (uint16_t)(relative % PROXY_LAZY_SECTORS_PER_CLUSTER);
	if (lazy_range_find(cluster, &range) != 0 ||
	    range.kind != PROXY_LAZY_RANGE_DATA) {
		return 0U;
	}
	last_cluster = (uint64_t)cluster +
		((uint64_t)sector_in_cluster + count - 1U) / PROXY_LAZY_SECTORS_PER_CLUSTER;
	if (last_cluster >= (uint64_t)range.base + range.clusters) {
		return 0U;
	}
	if (!lazy.dirs[range.dir_id].cache_ready) {
		*result = lazy_dir_load(range.dir_id);
		if (*result != 0) {
			return 0U;
		}
	}
	entry_index = lazy_entry_by_slot(&lazy.dirs[range.dir_id], range.slot);
	if (entry_index == UINT32_MAX ||
	    lazy.dirs[range.dir_id].entries[entry_index].deleted ||
	    lazy.dirs[range.dir_id].entries[entry_index].directory) {
		return 0U;
	}
	file_offset = ((uint64_t)range.offset + (cluster - range.base)) *
		PROXY_LAZY_CLUSTER_SIZE +
		(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;
	length = (uint64_t)count * PROXY_LAZY_SECTOR_SIZE;
	if (file_offset >= lazy.dirs[range.dir_id].entries[entry_index].size ||
	    length > lazy.dirs[range.dir_id].entries[entry_index].size - file_offset) {
		return 0U;
	}
	/* This write supersedes anything still staged for the same clusters. */
	for (uint32_t number = cluster; number <= last_cluster; number++) {
		lazy_pending_drop_cluster(number);
		lazy_run_drop_cluster(number);
	}
	*result = lazy_write_bytes(range.dir_id, entry_index, file_offset, input,
		(size_t)length);
	return count;
}

/**
 * @brief Block device write callback of the lazy view.
 *
 * @details The whole request runs under the view lock, one sector at a time.
 * @c lazy_write_sector() routes each sector: system areas are read-only, the
 * bitmap is updated, directory sectors are parsed, file sectors are written to
 * their source file, and free tail sectors wait for their entry.
 *
 * @param[in] disk   Disk being written; unused.
 * @param[in] sector First logical sector.
 * @param[in] buffer Source of the sectors.
 * @param[in] count  Number of sectors.
 * @return 0 on success, or the first failing sector's error.
 */
int lazy_disk_write(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count)
{
	const uint8_t *input = buffer;
	int result;

	(void)disk;
	if (!lazy_armed_enter()) {
		return -ENODEV;
	}
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result != 0) {
		lazy_armed_leave();
		return result;
	}
	if (sector > lazy.total_sectors ||
	    (uint64_t)count > lazy.total_sectors - sector) {
		mutex_unlock(&lazy.lock);
		lazy_armed_leave();
		return -EINVAL;
	}
	lazy.clean = false;
	for (uint32_t index = 0U; index < count;) {
		uint32_t consumed = lazy_write_data_run(sector + index,
			input + (size_t)index * PROXY_LAZY_SECTOR_SIZE, count - index, &result);

		if (consumed != 0U) {
			if (result != 0) {
				break;
			}
			index += consumed;
			continue;
		}
		result = lazy_write_sector(sector + index,
			input + (size_t)index * PROXY_LAZY_SECTOR_SIZE);
		if (result != 0) {
			break;
		}
		index++;
	}
	lazy.sectors_written += count;
	if (result != 0) {
		uint64_t relative = sector >= lazy.data_start ? sector - lazy.data_start : 0U;
		uint32_t cluster = sector >= lazy.data_start ?
			(uint32_t)(relative / PROXY_LAZY_SECTORS_PER_CLUSTER) + 2U : 0U;

		pr_warn("lazy proxy: write at sector %llu cluster %u failed: %d\n",
			(unsigned long long)sector, cluster, result);
	}
	mutex_unlock(&lazy.lock);
	lazy_armed_leave();
	return result;
}

/**
 * @brief Block device flush callback of the lazy view.
 *
 * @param[in] disk Disk being flushed; unused.
 * @return 0 on success, or -ENODEV after stop began.
 */
int lazy_disk_flush(struct gendisk *disk)
{
	int result;

	(void)disk;
	if (!lazy_armed_enter()) {
		return -ENODEV;
	}
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result == 0) {
		result = lazy_flush_locked();
		mutex_unlock(&lazy.lock);
	}
	lazy_armed_leave();
	return result;
}

/**
 * @brief Block device free callback of the lazy view.
 *
 * @param[in] disk Disk being freed; unused.
 */
void lazy_disk_free(struct gendisk *disk)
{
	(void)disk;
}

/** @brief Block device operations the lazy view registers under @c cardview1. */
const struct block_device_operations lazy_disk_operations = {
	.read = lazy_disk_read,
	.write = lazy_disk_write,
	.flush = lazy_disk_flush,
	.free_disk = lazy_disk_free,
};
