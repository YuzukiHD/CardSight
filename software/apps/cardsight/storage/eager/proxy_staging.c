// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_staging.c
 * @brief Sparse staging files, their claims and orphaned data.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/**
 * @brief Find the staging run that covers a cluster.
 *
 * @details A cluster belongs to a run from its first cluster up to its end
 * plus the configured gap, because the host may skip a few clusters.
 *
 * @param[in] cluster Cluster to look for.
 * @return The run, or NULL.
 */
struct proxy_run *proxy_find_run(uint32_t cluster)
{
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		struct proxy_run *run = &proxy.runs[index];

		if (run->used && cluster >= run->first_cluster && cluster < run->first_cluster + run->clusters + PROXY_RUN_GAP) {
			return run;
		}
	}
	return NULL;
}


/**
 * @brief Count the clusters of a run that no file has claimed.
 *
 * @details Clusters of a run that still wait for an entry. A tail cluster that
 * ends mid-way is never complete. Clusters past PROXY_RUN_TRACKED have no
 * claim bit and always count as unclaimed.
 *
 * @param[in] run Run to inspect.
 * @return Number of unclaimed clusters.
 */
uint32_t proxy_run_unclaimed(const struct proxy_run *run)
{
	uint32_t count = 0U;

	for (uint32_t index = 0U; index < run->clusters; index++) {
		if (index >= PROXY_RUN_TRACKED || (run->claimed[index / 8U] & (1U << (index % 8U))) == 0U) {
			count++;
		}
	}
	return count;
}


/**
 * @brief Open a staging run for clusters that no entry has claimed.
 *
 * @details The first writable mounted partition is used. All run slots busy:
 * the oldest run with at most its tail cluster left is as good as claimed and
 * is released. A free slot then gets a ".cs-stage-<cluster>" file, truncated if
 * it already exists, and the run starts empty at the given cluster.
 *
 * @param[in] cluster First cluster of the stream.
 * @return The new run, or NULL when no writable source or no free slot exists.
 */
struct proxy_run *proxy_new_run(uint32_t cluster)
{
	struct proxy_run *evict = NULL;

	uint32_t partition = PROXY_PARTITIONS;
	int descriptor;

	for (uint32_t index = 0U; index < PROXY_PARTITIONS; index++) {
		if (proxy.partitions[index].mounted && proxy.partitions[index].writable) {
			partition = index;
			break;
		}
	}
	if (partition == PROXY_PARTITIONS) {
		return NULL;
	}
	/* All slots busy: the oldest run with at most its tail cluster left is as good as claimed. */
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		if (!proxy.runs[index].used) {
			evict = NULL;
			break;
		}
		if (evict == NULL && proxy_run_unclaimed(&proxy.runs[index]) <= 1U) {
			evict = &proxy.runs[index];
		}
	}
	if (evict != NULL) {
		proxy_run_release(evict);
	}
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		struct proxy_run *run = &proxy.runs[index];

		if (run->used) {
			continue;
		}
		{
			char path[sizeof(run->path)];

			snprintf(path, sizeof(path), "%s/" PROXY_STAGE_PREFIX "%u", proxy.partitions[partition].mount, cluster);
			memcpy(run->path, path, sizeof(path));
		}
		(void)vfs_unlink(run->path);
		descriptor = vfs_open(run->path, VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
		if (descriptor < 0) {
			return NULL;
		}
		(void)vfs_close(descriptor);
		run->used = true;
		run->first_cluster = cluster;
		run->clusters = 0U;
		memset(run->claimed, 0, sizeof(run->claimed));
		run->partition = partition;
		return run;
	}
	return NULL;
}


/**
 * @brief Move the RAM data of clusters [start, end) into the run's file.
 *
 * @details The staged stream continues the same offsets, so every waiting
 * sector of the range is queued at its run-relative offset, marked unused and
 * counted as staged. The run's length becomes the end of the moved range.
 *
 * @param[in,out] run   Run receiving the data.
 * @param[in]     start First cluster to move.
 * @param[in]     end   One past the last cluster to move.
 * @return 0 on success, or a negative errno.
 */
int proxy_migrate_pending(struct proxy_run *run, uint32_t start, uint32_t end)
{
	for (uint32_t index = 0U; index < proxy.pending_count; index++) {
		struct proxy_pending_sector *pending = &proxy.pending[index];
		int result;

		if (!pending->used || pending->cluster < start || pending->cluster >= end) {
			continue;
		}
		result = proxy_batch_write(run->path, ((uint64_t)(pending->cluster - start) * PROXY_SECTORS_PER_CLUSTER +
			pending->sector) * PROXY_SECTOR_SIZE, pending->data);
		if (result != 0) {
			return result;
		}
		pending->used = false;
		proxy_stats.staged_sectors++;
	}
	run->clusters = end - start;
	return 0;
}


/**
 * @brief Store a sector that has no directory entry yet.
 *
 * @details A host writes file data before the entry that owns it, often
 * megabytes of it. Small amounts wait in RAM; beyond that the sectors go to a
 * staging file on the source card until an entry claims them. A cluster that
 * already has a run appends there; otherwise, while the RAM table has room,
 * the sector waits in RAM. When the table is full a run is created starting
 * where the stream began in RAM, so the whole file is one run and can be
 * renamed into place; the RAM sectors of the earlier part are migrated into
 * it first. Data for a cluster that was already claimed clears its claim bit,
 * because the file shrank and the data must be copied again.
 *
 * @param[in] cluster Cluster the sector belongs to.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, or a negative errno.
 */
int proxy_orphan_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE])
{
	struct proxy_run *run = proxy_find_run(cluster);
	uint32_t extent;

	if (run == NULL && proxy.pending_count < PROXY_PENDING_MAX) {
		return proxy_pending_store(cluster, sector, data);
	}
	if (run == NULL) {
		/* The run starts where this stream began in RAM, so the whole file is one run and can be renamed. */
		uint32_t start = cluster;
		int result;

		while (start > 2U && proxy_pending_has_cluster(start - 1U)) {
			start--;
		}
		run = proxy_new_run(start);
		if (run == NULL) {
			return -ENOSPC;
		}
		result = proxy_migrate_pending(run, start, cluster);
		if (result != 0) {
			return result;
		}
	}
	proxy_pending_drop(cluster, sector);
	/* New data for a cluster that was claimed before (its file shrank): it must be copied again. */
	if (cluster - run->first_cluster < PROXY_RUN_TRACKED) {
		uint32_t tracked = cluster - run->first_cluster;

		run->claimed[tracked / 8U] &= (uint8_t)~(1U << (tracked % 8U));
	}
	extent = cluster - run->first_cluster + 1U;
	if (extent > run->clusters) {
		run->clusters = extent;
	}
	proxy_stats.staged_sectors++;
	return proxy_batch_write(run->path, ((uint64_t)(cluster - run->first_cluster) * PROXY_SECTORS_PER_CLUSTER + sector) *
		PROXY_SECTOR_SIZE, data);
}


/**
 * @brief Release a staging run and delete its staging file.
 *
 * @details A queued write still points at this path buffer, which the next run
 * reuses: it is dropped with the file. The open staging descriptor is closed
 * when it belongs to the run, the file is unlinked unless the source card is
 * already gone, and the slot is marked free.
 *
 * @param[in,out] run Run to release.
 */
void proxy_run_release(struct proxy_run *run)
{
	/* A queued write still points at this path buffer, which the next run reuses: drop it with the file. */
	if (proxy.write_path == run->path) {
		proxy.write_length = 0U;
	}
	if (proxy.stage_path == run->path) {
		proxy_stage_close();
	}
	if (run->used && !proxy.source_lost) {
		(void)vfs_unlink(run->path);
	}
	run->used = false;
}


/**
 * @brief Report whether one cluster of a run was already copied.
 *
 * @details Clusters past PROXY_RUN_TRACKED have no bit and read as not
 * claimed.
 *
 * @param[in] run     Run to inspect.
 * @param[in] cluster Cluster to test.
 * @return True when the cluster's claim bit is set.
 */
bool proxy_run_claimed(const struct proxy_run *run, uint32_t cluster)
{
	uint32_t index = cluster - run->first_cluster;

	return index < PROXY_RUN_TRACKED && (run->claimed[index / 8U] & (1U << (index % 8U))) != 0U;
}


/**
 * @brief Report whether every cluster of a run was copied to its file.
 *
 * @details A run longer than PROXY_RUN_TRACKED returns false because its
 * claims cannot be tracked; it stays until the proxy stops.
 *
 * @param[in] run Run to inspect.
 * @return True when every tracked cluster is claimed.
 */
bool proxy_run_all_claimed(const struct proxy_run *run)
{
	if (run->clusters > PROXY_RUN_TRACKED) {
		return false; /* too long to track: it stays until the proxy stops */
	}
	for (uint32_t index = 0U; index < run->clusters; index++) {
		if ((run->claimed[index / 8U] & (1U << (index % 8U))) == 0U) {
			return false;
		}
	}
	return true;
}


/**
 * @brief Copy clusters [low, high) of a run into the file that owns them.
 *
 * @details The target range is the cluster range translated to file offsets
 * and clamped to the node's size. When the range is non-empty, the staging file
 * is opened for reading and the target for writing; the target is filled with
 * zeros up to the start, then copied in buffer-sized chunks. The claim bits
 * are set only for clusters the size covers in full, so a last partial cluster
 * is copied again when the size grows. Returns what is wrong, if anything.
 *
 * @param[in,out] run  Run holding the staged data.
 * @param[in]     node Node that owns the clusters.
 * @param[in]     low  First cluster to copy.
 * @param[in]     high One past the last cluster to copy.
 * @return 0 on success, or a negative errno.
 */
int proxy_copy_run_range(struct proxy_run *run, const struct proxy_node *node, uint32_t low, uint32_t high)
{
	const char *node_path = proxy_string(node->source_offset);
	uint64_t from = (uint64_t)(low - node->first_cluster) * PROXY_CLUSTER_SIZE;
	uint64_t to = (uint64_t)(high - node->first_cluster) * PROXY_CLUSTER_SIZE;
	uint64_t source_base = (uint64_t)(low - run->first_cluster) * PROXY_CLUSTER_SIZE;
	uint32_t whole = node->first_cluster + (uint32_t)(node->size / PROXY_CLUSTER_SIZE);
	int source;
	int target;
	int result = 0;

	if (to > node->size) {
		to = node->size;
	}
	if (from < to) {
		source = vfs_open(run->path, VFS_O_READ);
		target = source < 0 ? source : vfs_open(node_path, VFS_O_READ | VFS_O_WRITE);
		if (source < 0 || target < 0) {
			pr_warn("proxy: staged copy cannot open: source %d target %d\n", source, target);
			if (source >= 0) {
				(void)vfs_close(source);
			}
			return source < 0 ? source : target;
		}
		result = proxy_fill_to(target, from);
		for (uint64_t position = from; result == 0 && position < to;) {
			size_t length = to - position < sizeof(proxy.write_buffer) ? (size_t)(to - position) : sizeof(proxy.write_buffer);
			int done;

			result = vfs_lseek(source, source_base + (position - from), 0) < 0 ? -EIO : 0;
			done = result == 0 ? vfs_read(source, proxy.write_buffer, length) : 0;
			if (result == 0 && done <= 0) {
				result = done < 0 ? done : -EIO;
			}
			if (result == 0) {
				result = vfs_lseek(target, position, 0) < 0 ? -EIO : 0;
			}
			if (result == 0 && vfs_write(target, proxy.write_buffer, (size_t)done) != done) {
				result = -EIO;
			}
			position += (uint64_t)done;
		}
		(void)vfs_close(source);
		{
			int close_result = vfs_close(target);

			result = result == 0 ? close_result : result;
		}
		if (result != 0) {
			pr_warn("proxy: staged copy failed: %d\n", result);
			return result;
		}
		proxy_stats.runs_copied++;
	}
	/* Only clusters the size covers in full are done; a last partial one is copied again when the size grows. */
	for (uint32_t cluster = low; cluster < high && cluster < whole; cluster++) {
		uint32_t index = cluster - run->first_cluster;

		if (index < PROXY_RUN_TRACKED) {
			run->claimed[index / 8U] |= (uint8_t)(1U << (index % 8U));
		}
	}
	return 0;
}


/**
 * @brief Give the staged data of a node's clusters to its file.
 *
 * @details The directory entry arrived: its clusters take the staged data, by
 * rename when it is all of the run. For every run that overlaps the node's
 * extent, the queued writes are flushed, the staging descriptor is closed and
 * the read cache dropped, then:
 * - when the node covers exactly the run on the same partition, the staging
 *   file itself becomes the source file by unlink and rename, and is truncated
 *   to the node's size;
 * - otherwise each still-unclaimed stretch is copied by
 *   @c proxy_copy_run_range().
 * A run whose every cluster was claimed is released.
 *
 * @param[in] node_index Node whose entry just arrived.
 * @return 0 on success, or a negative errno.
 */
int proxy_claim_runs(uint32_t node_index)
{
	struct proxy_node *node = &proxy.nodes[node_index];
	const char *node_path;

	if (node->source != PROXY_SOURCE_VFS || node->directory || node->cluster_count == 0U || node->size == 0U) {
		return 0;
	}
	node_path = proxy_string(node->source_offset);
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		struct proxy_run *run = &proxy.runs[index];
		uint32_t low;
		uint32_t high;
		int result;

		if (!run->used) {
			continue;
		}
		low = node->first_cluster > run->first_cluster ? node->first_cluster : run->first_cluster;
		high = node->first_cluster + node->cluster_count < run->first_cluster + run->clusters ?
			node->first_cluster + node->cluster_count : run->first_cluster + run->clusters;
		if (low >= high) {
			continue;
		}
		result = proxy_write_flush();
		if (result != 0) {
			return result;
		}
		proxy_stage_close();
		proxy_drop_read_cache();
		if (node->first_cluster == run->first_cluster && node->cluster_count == run->clusters &&
			proxy_path_partition(node_path) == run->partition) {
			int descriptor;

			(void)vfs_unlink(node_path);
			result = vfs_rename(run->path, node_path);
			if (result != 0) {
				pr_warn("proxy: staged file rename failed: %d\n", result);
				return result;
			}
			run->used = false;
			proxy_stats.runs_renamed++;
			descriptor = vfs_open(node_path, VFS_O_WRITE);
			if (descriptor >= 0) {
				result = vfs_ftruncate(descriptor, node->size);
				(void)vfs_close(descriptor);
			} else {
				result = descriptor;
			}
			if (result != 0) {
				pr_warn("proxy: staged file truncate failed: %d\n", result);
				return result;
			}
			continue;
		}
		/* Copy each stretch that has not been copied yet. */
		for (uint32_t cluster = low; cluster < high;) {
			uint32_t end;

			if (proxy_run_claimed(run, cluster)) {
				cluster++;
				continue;
			}
			end = cluster;
			while (end < high && !proxy_run_claimed(run, end)) {
				end++;
			}
			result = proxy_copy_run_range(run, node, cluster, end);
			if (result != 0) {
				return result;
			}
			cluster = end;
		}
		if (proxy_run_all_claimed(run)) {
			proxy_run_release(run);
		}
	}
	return 0;
}
