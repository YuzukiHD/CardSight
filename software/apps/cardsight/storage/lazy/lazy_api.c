// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_api.c
 * @brief Public query, test and layout accessors of the lazy view.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Report whether the lazy view is registered as a block device.
 *
 * @return True while @c cardview1 exists.
 */
bool storage_proxy_lazy_running(void)
{
	return lazy.running;
}

/**
 * @brief Return the block device name the lazy view is registered under.
 *
 * @return The constant device name of the lazy view.
 */
const char *storage_proxy_lazy_device_name(void)
{
	return PROXY_LAZY_DISK_NAME;
}

/**
 * @brief Return the number of source directories the view has indexed.
 *
 * @return Directory header count, including the root and the partition folders.
 */
size_t storage_proxy_lazy_directory_count(void)
{
	size_t count = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		count = lazy.dir_count;
		mutex_unlock(&lazy.lock);
	}
	return count;
}

/**
 * @brief Return the number of directory entries currently cached.
 *
 * @return Cached entry count.
 */
uint32_t storage_proxy_lazy_cached_entries(void)
{
	uint32_t count = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		count = lazy.cache_entries;
		mutex_unlock(&lazy.lock);
	}
	return count;
}

/**
 * @brief Return the number of bytes the directory entry caches currently hold.
 *
 * @return Cached byte count, including names and cluster order arrays.
 */
uint32_t storage_proxy_lazy_cached_bytes(void)
{
	uint32_t bytes = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		bytes = lazy.cache_bytes;
		mutex_unlock(&lazy.lock);
	}
	return bytes;
}

/**
 * @brief Return the number of entry caches evicted since start.
 *
 * @return Eviction count.
 */
uint32_t storage_proxy_lazy_cache_evictions(void)
{
	uint32_t count = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		count = lazy.evictions;
		mutex_unlock(&lazy.lock);
	}
	return count;
}

/**
 * @brief Report whether any index limit made part of the source tree invisible.
 *
 * @return True when the view is truncated.
 */
bool storage_proxy_lazy_truncated(void)
{
	bool truncated = false;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		truncated = lazy.truncated;
		mutex_unlock(&lazy.lock);
	}
	return truncated;
}

/**
 * @brief Install a directory indexing progress callback.
 *
 * @details Set this before starting the view. The callback runs under the view
 * lock and must only request cancellation through @c storage_proxy_lazy_cancel().
 *
 * @param[in] callback Receives the entry count and source path, or NULL.
 * @param[in] context  Opaque pointer passed to @p callback.
 */
void storage_proxy_lazy_set_progress(void (*callback)(void *context, uint32_t entries,
	const char *path), void *context)
{
	lazy.progress_callback = callback;
	lazy.progress_context = context;
}

/**
 * @brief Ask a running directory count or load to stop at its next check.
 */
void storage_proxy_lazy_cancel(void)
{
	(void)__atomic_store_n(&lazy.cancel, true, __ATOMIC_SEQ_CST);
}
#ifdef STORAGE_PROXY_HOST_TEST
/**
 * @brief Override the runtime cache and directory caps for a host test.
 *
 * @param[in] cache_entries   Entries all LRU caches may hold together, or zero.
 * @param[in] cache_bytes     Bytes all LRU caches may hold together, or zero.
 * @param[in] dir_max_entries Entries one source directory may expose, or zero.
 */
void storage_proxy_lazy_test_limits(uint32_t cache_entries, uint32_t cache_bytes,
	uint32_t dir_max_entries)
{
	lazy.cache_entries_max = cache_entries != 0U ? cache_entries :
		CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES;
	lazy.cache_bytes_max = cache_bytes != 0U ? cache_bytes :
		CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES;
	lazy.dir_max_entries = dir_max_entries != 0U ? dir_max_entries :
		CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES;
}

/**
 * @brief Snapshot the write staging counters for a host test.
 *
 * @param[out] stats Receives the counters; cleared when the view is stopped.
 */
void storage_proxy_lazy_test_stats(struct storage_proxy_lazy_test_stats *stats)
{
	uint32_t runs = 0U;

	memset(stats, 0, sizeof(*stats));
	if (lazy.runs != NULL) {
		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
			if (lazy.runs[index].used) {
				runs++;
			}
		}
	}
	stats->pending_sectors = lazy.pending_count;
	stats->active_runs = runs;
	stats->staged_sectors = lazy.staged_sectors;
}
#endif

/**
 * @brief Drop every cached directory entry, keeping extents and the overlay.
 *
 * @details The next read reloads each directory from the source readdir result
 * merged with the overlay. Sizes are first enforced so an evicted cache does
 * not leave a source file longer than the entry the host announced.
 */
void storage_proxy_lazy_cache_drop(void)
{
	if (mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS) != 0) {
		return;
	}
	(void)lazy_enforce_sizes_locked();
	while (lazy.lru_head != PROXY_LAZY_INVALID_DIR) {
		lazy_cache_evict(lazy.lru_head);
	}
	mutex_unlock(&lazy.lock);
}

/**
 * @brief Copy the geometry and reservation regions of the running view.
 *
 * @param[out] layout Receives the layout.
 * @return 0 on success, -EINVAL for a NULL argument, -ENODEV when the view is
 *         stopped, or -ETIMEDOUT when the view lock stayed busy.
 */
int storage_proxy_lazy_layout(struct storage_proxy_lazy_layout *layout)
{
	int result;

	if (layout == NULL) {
		return -EINVAL;
	}
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result != 0) {
		return result;
	}
	if (!lazy.running) {
		mutex_unlock(&lazy.lock);
		return -ENODEV;
	}
	memset(layout, 0, sizeof(*layout));
	layout->total_clusters = lazy.total_clusters;
	layout->cluster_bytes = (uint32_t)PROXY_LAZY_CLUSTER_SIZE;
	layout->data_start = lazy.data_start;
	layout->bitmap_cluster = lazy.bitmap_cluster;
	layout->bitmap_bytes = lazy.bitmap_bytes;
	layout->root_cluster = lazy.root_cluster;
	layout->root_clusters = lazy.root_clusters;
	layout->tail_first = lazy.tail_first;
	layout->tail_clusters = lazy.tail_clusters;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		const struct proxy_lazy_partition *part = &lazy.partitions[partition];

		layout->selected[partition] = part->selected;
		layout->reserve_first[partition] = part->reserve_first;
		layout->reserve_clusters[partition] = part->reserve_clusters;
		if (part->selected && part->dir_id < lazy.dir_count &&
		    lazy.dirs[part->dir_id].extent_ready) {
			layout->partition_root_first[partition] = lazy.dirs[part->dir_id].first_cluster;
			layout->partition_root_clusters[partition] = lazy.dirs[part->dir_id].clusters;
		}
	}
	mutex_unlock(&lazy.lock);
	return 0;
}
