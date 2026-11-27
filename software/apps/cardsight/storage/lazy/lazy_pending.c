// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_pending.c
 * @brief RAM pending table for host data and orphan handling.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Store one host sector until its directory entry arrives.
 *
 * @param[in] cluster View cluster.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, or -ENOSPC when the table is full.
 */
int lazy_pending_store(uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		struct proxy_lazy_pending_sector *pending = &lazy.pending[index];

		if (pending->cluster == cluster && pending->sector == sector) {
			memcpy(pending->data, data, PROXY_LAZY_SECTOR_SIZE);
			pending->used = true;
			return 0;
		}
	}
	if (lazy.pending_count >= CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS) {
		return -ENOSPC;
	}
	{
		struct proxy_lazy_pending_sector *pending = &lazy.pending[lazy.pending_count++];

		pending->cluster = cluster;
		pending->sector = sector;
		pending->used = true;
		memcpy(pending->data, data, PROXY_LAZY_SECTOR_SIZE);
	}
	return 0;
}

/**
 * @brief Forget every pending sector of a cluster the host freed.
 *
 * @param[in] cluster Cluster that became free.
 */
void lazy_pending_drop_cluster(uint32_t cluster)
{
	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		if (lazy.pending[index].cluster == cluster) {
			lazy.pending[index].used = false;
		}
	}
}

/**
 * @brief Drop unused entries from the front of the pending table.
 *
 * @details Migrating a stream to a staging file retires most of the table;
 * compacting it keeps the RAM limit meaningful for later bursts.
 */
void lazy_pending_compact(void)
{
	uint32_t kept = 0U;

	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		if (!lazy.pending[index].used) {
			continue;
		}
		if (kept != index) {
			lazy.pending[kept] = lazy.pending[index];
		}
		kept++;
	}
	lazy.pending_count = kept;
}

/**
 * @brief Report whether the pending table holds any sector of a cluster.
 *
 * @param[in] cluster Cluster to look for.
 * @return True when a sector of the cluster is waiting.
 */
bool lazy_pending_has_cluster(uint32_t cluster)
{
	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		if (lazy.pending[index].used && lazy.pending[index].cluster == cluster) {
			return true;
		}
	}
	return false;
}

/**
 * @brief Store a host sector whose directory entry has not arrived yet.
 *
 * @details Small bursts wait in the RAM pending table. When the table is full
 * the whole stream, including the sectors already in RAM, moves to a staging
 * file on the source card; further sectors append there. A stream that already
 * has a staging run keeps using it. This is what lets a host write a large file
 * data-first without the proxy holding the file in heap.
 *
 * @param[in] cluster View cluster.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, or a negative errno.
 */
int lazy_orphan_store(uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	struct proxy_lazy_run *run = lazy_run_find(cluster);

	if (run == NULL) {
		run = lazy_run_extendable(cluster);
	}
	if (run != NULL) {
		return lazy_run_write(run, cluster, sector, data);
	}
	if (lazy.pending_count < CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS) {
		return lazy_pending_store(cluster, sector, data);
	}
	{
		uint32_t start = cluster;

		while (start > PROXY_LAZY_ROOT_CLUSTER_MIN &&
		       lazy_pending_has_cluster(start - 1U)) {
			start--;
		}
		run = lazy_run_create(start);
		if (run == NULL) {
			return -ENOSPC;
		}
		for (uint32_t index = 0U; index < lazy.pending_count; index++) {
			struct proxy_lazy_pending_sector *pending = &lazy.pending[index];
			int result;

			/*
			 * Include the current cluster: its earlier sectors are still in
			 * RAM, and the staging file must hold them too or the claim would
			 * read sparse zeros over the data that is still pending.
			 */
			if (!pending->used || pending->cluster < start ||
			    pending->cluster > cluster) {
				continue;
			}
			result = lazy_run_write(run, pending->cluster, pending->sector,
				pending->data);
			if (result != 0) {
				return result;
			}
			pending->used = false;
		}
		lazy_pending_compact();
	}
	return lazy_run_write(run, cluster, sector, data);
}

/**
 * @brief Forget the extent of a deleted entry whose clusters the host reused.
 *
 * @details A deleted entry keeps its extent so a host that clears the old group
 * before writing the renamed one is still matched. Once the bitmap frees the
 * clusters, though, a new object may receive them, and matching the old entry
 * to the new object would be wrong.
 *
 * @param[in] cluster Cluster that became free.
 */
void lazy_forget_freed_cluster(uint32_t cluster)
{
	for (uint32_t dir = 0U; dir < lazy.dir_count; dir++) {
		struct proxy_lazy_dir *header = &lazy.dirs[dir];

		if (!header->cache_ready || header->entries == NULL) {
			continue;
		}
		for (uint32_t index = 0U; index < header->entry_count; index++) {
			struct proxy_lazy_dirent *item = &header->entries[index];

			if (item->deleted && item->clusters != 0U &&
			    cluster >= item->first_cluster &&
			    cluster < item->first_cluster + item->clusters) {
				item->first_cluster = 0U;
				item->clusters = 0U;
				header->ranges_dirty = true;
			}
		}
	}
}
