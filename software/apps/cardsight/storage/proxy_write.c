// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_write.c
 * @brief Host writes: data that arrives before its entry, staging on the card, claims and deletes.
 *
 * The host may write file data before the directory entry that describes it,
 * and in an order that is not sequential. Small amounts wait in RAM keyed by
 * (cluster, sector); past that limit the data goes to a staging file on the
 * source card. When the directory entry finally arrives, the staged clusters
 * are renamed or copied into the file that owns them. Deletes are carried out
 * at flush time. Everything here runs under @c proxy.lock.
 */

#include "proxy_internal.h"


/**
 * @brief Report whether a path is the staging file of an open run.
 *
 * @details The comparison is by pointer, because the path buffer of a run is
 * what the batch queue holds while it points at a staging file.
 *
 * @param[in] path Path to test.
 * @return True when @p path is one of the run paths.
 */
static bool proxy_is_stage_path(const char *path)
{
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		if (path == proxy.runs[index].path) {
			return true;
		}
	}
	return false;
}


/**
 * @brief Close the staging file kept open for appends.
 *
 * @details The cached path is cleared with the descriptor, so the next flush
 * reopens the file and seeks explicitly.
 */
void proxy_stage_close(void)
{
	if (proxy.stage_fd >= 0) {
		(void)vfs_close(proxy.stage_fd);
		proxy.stage_fd = -1;
	}
	proxy.stage_path = NULL;
}


/**
 * @brief Write the batched sectors to their file.
 *
 * @details Write the waiting run to its file with one open, one seek and as
 * few writes as the buffer allows. The length is cleared before the work so a
 * failure cannot flush the same data twice, and the read cache is dropped
 * because the file just changed. A staging file keeps its descriptor open and
 * reuses its position; an ordinary source file is opened, filled with zeros up
 * to the offset if it is shorter, written and closed. The bytes are counted
 * even when the write fails, and a failed staging append closes the descriptor
 * so the next attempt starts clean.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_write_flush(void)
{
	uint32_t length = proxy.write_length;
	uint32_t done = 0U;
	uint64_t begin;
	bool staged;
	int descriptor;
	int result = 0;

	if (length == 0U) {
		return 0;
	}
	proxy.write_length = 0U;
	if (proxy.source_lost) {
		return -ENODEV;
	}
	staged = proxy_is_stage_path(proxy.write_path);
	if (!staged) {
		proxy_stage_close();
	}
	proxy_drop_read_cache();
	begin = proxy_time_begin();
	proxy_stats.file_bytes += length;
	if (staged && proxy.stage_fd >= 0 && proxy.stage_path == proxy.write_path) {
		descriptor = proxy.stage_fd;
	} else {
		proxy_stage_close();
		descriptor = vfs_open(proxy.write_path, VFS_O_READ | VFS_O_WRITE);
		if (descriptor < 0) {
			return descriptor;
		}
		if (staged) {
			proxy.stage_fd = descriptor;
			proxy.stage_path = proxy.write_path;
			proxy.stage_position = UINT64_MAX;
		}
	}
	result = proxy_fill_to(descriptor, proxy.write_offset);
	if (result == 0 && (!staged || proxy.stage_position != proxy.write_offset)) {
		result = vfs_lseek(descriptor, proxy.write_offset, 0) < 0 ? -EIO : 0;
	}
	while (result == 0 && done < length) {
		int written = vfs_write(descriptor, proxy.write_buffer + done, length - done);

		if (written <= 0) {
			result = written < 0 ? written : -EIO;
		} else {
			done += (uint32_t)written;
		}
	}
	if (staged) {
		proxy.stage_position = result == 0 ? proxy.write_offset + length : UINT64_MAX;
		if (result != 0) {
			proxy_stage_close();
		}
		proxy_time_end(&proxy_stats.batch_flush, begin);
		return result;
	}
	{
		int close_result = vfs_close(descriptor);

		proxy_time_end(&proxy_stats.batch_flush, begin);
		return result == 0 ? close_result : result;
	}
}


/**
 * @brief Queue a sector for a file; consecutive sectors of one file leave as a single write.
 *
 * @details A sector is appended to the batch buffer when it continues the same
 * file at the next offset and the buffer has room; otherwise the buffer is
 * flushed first and the new run starts. The path pointer is stored, not copied,
 * which is why a release of the batch must happen before a run's path is
 * reused.
 *
 * @param[in] path   Source path of the file.
 * @param[in] offset Byte offset of the sector inside the file.
 * @param[in] data   Sector contents.
 * @return 0 on success, or a negative errno from the flush.
 */
static int proxy_batch_write(const char *path, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE])
{
	if (proxy.write_length != 0U && (proxy.write_path != path ||
		proxy.write_offset + proxy.write_length != offset || proxy.write_length == sizeof(proxy.write_buffer))) {
		int result = proxy_write_flush();

		if (result != 0) {
			return result;
		}
	}
	if (proxy.write_length == 0U) {
		proxy.write_path = path;
		proxy.write_offset = offset;
	}
	memcpy(proxy.write_buffer + proxy.write_length, data, PROXY_SECTOR_SIZE);
	proxy.write_length += PROXY_SECTOR_SIZE;
	return 0;
}


/**
 * @brief Queue one sector of a file for write-back.
 *
 * @details Volatile nodes accept and discard the data. Directories and nodes
 * without a source file are read-only here. Everything else goes through the
 * batch buffer with the node's source path.
 *
 * @param[in] node_index Node the sector belongs to.
 * @param[in] offset     Byte offset inside the file.
 * @param[in] data       Sector contents.
 * @return 0 on success, or a negative errno.
 */
int proxy_write_node(uint32_t node_index, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE])
{
	struct proxy_node *node = &proxy.nodes[node_index];

	if (node->source == PROXY_SOURCE_VOLATILE) {
		return 0;
	}
	if (node->source != PROXY_SOURCE_VFS || node->directory) {
		return -EROFS;
	}
	return proxy_batch_write(proxy_string(node->source_offset), offset, data);
}


/**
 * @brief Hash a (cluster, sector) pair into the pending index.
 *
 * @details The pair is packed, multiplied by a large odd constant and shifted
 * right, which spreads neighbouring sectors across the table.
 *
 * @param[in] cluster Cluster of the sector.
 * @param[in] sector  Sector offset inside the cluster.
 * @return Hash value; the caller masks it with the table size.
 */
static uint32_t proxy_pending_hash(uint32_t cluster, uint16_t sector)
{
	return ((cluster * 64U + sector) * 2654435761U) >> 7;
}


/**
 * @brief Rebuild the pending hash index from the array.
 *
 * @details The table is sized to at least twice the capacity so probe
 * sequences stay short, then every live entry is inserted with linear probing.
 * The old table is freed only after the new one is complete. Rebuild the index
 * from the array; after a flush compacts it or the array grows.
 *
 * @param[in] capacity Capacity of the pending array.
 * @return 0 on success, or -ENOMEM.
 */
static int proxy_pending_reindex(uint32_t capacity)
{
	uint32_t size = 1U;
	uint32_t *index;

	while (size < capacity * 2U) {
		size <<= 1;
	}
	index = proxy_calloc(size, sizeof(*index));
	if (index == NULL) {
		return -ENOMEM;
	}
	for (uint32_t entry = 0U; entry < proxy.pending_count; entry++) {
		uint32_t position = proxy_pending_hash(proxy.pending[entry].cluster, proxy.pending[entry].sector) & (size - 1U);

		while (index[position] != 0U) {
			position = (position + 1U) & (size - 1U);
		}
		index[position] = entry + 1U;
	}
	proxy_free(proxy.pending_index);
	proxy.pending_index = index;
	proxy.pending_index_size = size;
	return 0;
}


/**
 * @brief Store one sector in the pending table.
 *
 * @details A rewrite of a sector already waiting replaces its contents in
 * place. When the array is full it doubles, up to PROXY_PENDING_MAX, copies
 * the live entries and rebuilds the hash index; at the limit the caller is
 * told there is no space and must fall back to staging on the card. A new
 * entry is appended and its index slot filled by linear probing.
 *
 * @param[in] cluster Cluster of the sector.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, -ENOSPC at the limit, or -ENOMEM.
 */
static int proxy_pending_store_inner(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE])
{
	struct proxy_pending_sector *pending;
	uint32_t position;

	/* A rewrite of the same sector replaces it. */
	if (proxy.pending_index_size != 0U) {
		position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
		while (proxy.pending_index[position] != 0U) {
			pending = &proxy.pending[proxy.pending_index[position] - 1U];
			if (pending->cluster == cluster && pending->sector == sector) {
				memcpy(pending->data, data, PROXY_SECTOR_SIZE);
				pending->used = true;
				return 0;
			}
			position = (position + 1U) & (proxy.pending_index_size - 1U);
		}
	}
	if (proxy.pending_count == proxy.pending_capacity) {
		uint32_t capacity = proxy.pending_capacity == 0U ? 256U : proxy.pending_capacity * 2U;
		struct proxy_pending_sector *grown;

		if (capacity > PROXY_PENDING_MAX) {
			capacity = PROXY_PENDING_MAX;
		}
		if (capacity == proxy.pending_capacity) {
			return -ENOSPC;
		}
		grown = proxy_alloc((size_t)capacity * sizeof(*grown));
		if (grown == NULL) {
			return -ENOMEM;
		}
		if (proxy.pending_count != 0U) {
			memcpy(grown, proxy.pending, (size_t)proxy.pending_count * sizeof(*grown));
		}
		proxy_free(proxy.pending);
		proxy.pending = grown;
		proxy.pending_capacity = capacity;
		if (proxy_pending_reindex(capacity) != 0) {
			return -ENOMEM;
		}
	}
	pending = &proxy.pending[proxy.pending_count++];
	if (proxy.pending_count > proxy_stats.pending_peak) {
		proxy_stats.pending_peak = proxy.pending_count;
	}
	pending->used = true;
	pending->cluster = cluster;
	pending->sector = sector;
	memcpy(pending->data, data, PROXY_SECTOR_SIZE);
	position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
	while (proxy.pending_index[position] != 0U) {
		position = (position + 1U) & (proxy.pending_index_size - 1U);
	}
	proxy.pending_index[position] = proxy.pending_count;
	return 0;
}


/**
 * @brief Store one sector in the pending table, with pending-store timing.
 *
 * @param[in] cluster Cluster of the sector.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, -ENOSPC or -ENOMEM.
 */
static int proxy_pending_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE])
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_pending_store_inner(cluster, sector, data);

	proxy_time_end(&proxy_stats.pending_store, begin);
	return result;
}


/**
 * @brief Write the pending sectors that belong to one node.
 *
 * @details Sectors that belong to the file are written in order, the rest stay
 * for another entry. The array is compacted in place while it is walked: a
 * sector is dropped once it is written, and everything else is kept. Entries
 * whose cluster was reused or freed are already marked unused and are dropped
 * without a write. Directory nodes only drop their sectors. The hash index is
 * rebuilt and the batch buffer flushed at the end.
 *
 * @param[in] node_index Node whose extent claims the sectors.
 * @return 0 on success, or a negative errno.
 */
static int proxy_pending_flush_inner(uint32_t node_index)
{
	struct proxy_node *node = &proxy.nodes[node_index];
	uint32_t kept = 0U;
	int result = 0;

	for (uint32_t index = 0U; index < proxy.pending_count; index++) {
		struct proxy_pending_sector *pending = &proxy.pending[index];
		bool mine = pending->cluster >= node->first_cluster &&
			pending->cluster < node->first_cluster + node->cluster_count;

		if (!pending->used) {
			continue; /* dropped: its cluster was reused or freed */
		}
		if (mine && result == 0 && !node->directory) {
			result = proxy_write_node(node_index,
				((uint64_t)(pending->cluster - node->first_cluster) * PROXY_SECTORS_PER_CLUSTER +
					pending->sector) * PROXY_SECTOR_SIZE, pending->data);
			if (result == 0) {
				continue;
			}
		} else if (mine && node->directory) {
			continue;
		}
		if (kept != index) {
			proxy.pending[kept] = *pending;
		}
		kept++;
	}
	proxy.pending_count = kept;
	if (proxy.pending_capacity != 0U && proxy_pending_reindex(proxy.pending_capacity) != 0 && result == 0) {
		result = -ENOMEM;
	}
	if (result == 0) {
		result = proxy_write_flush();
	}
	return result;
}


/**
 * @brief Write the pending sectors of one node, with pending-flush timing.
 *
 * @param[in] node_index Node whose extent claims the sectors.
 * @return 0 on success, or a negative errno.
 */
static int proxy_pending_flush(uint32_t node_index)
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_pending_flush_inner(node_index);
	proxy_time_end(&proxy_stats.pending_flush, begin);
	return result;
}


/**
 * @brief Find the source partition a path belongs to.
 *
 * @details The mount targets are compared as prefixes and the character after
 * the prefix must be a separator or the end of the path, so "/proxy1" does not
 * match "/proxy10".
 *
 * @param[in] path Source path.
 * @return Partition index, or PROXY_PARTITIONS when none matches.
 */
static uint32_t proxy_path_partition(const char *path)
{
	for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
		size_t length = strlen(proxy.partitions[partition].mount);

		if (proxy.partitions[partition].mounted && length != 0U &&
			strncmp(path, proxy.partitions[partition].mount, length) == 0 &&
			(path[length] == '/' || path[length] == '\0')) {
			return partition;
		}
	}
	return PROXY_PARTITIONS;
}


/**
 * @brief Report whether any sector of a cluster is waiting in RAM.
 *
 * @details Only every eighth sector is probed, which is enough to decide
 * whether a cluster is part of the current in-order stream; a false negative
 * only makes the staging run start earlier.
 *
 * @param[in] cluster Cluster to look for.
 * @return True when a waiting sector of the cluster was found.
 */
static bool proxy_pending_has_cluster(uint32_t cluster)
{
	for (uint16_t sector = 0U; sector < PROXY_SECTORS_PER_CLUSTER; sector += 8U) {
		uint32_t position;

		if (proxy.pending_index_size == 0U) {
			return false;
		}
		position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
		while (proxy.pending_index[position] != 0U) {
			const struct proxy_pending_sector *pending = &proxy.pending[proxy.pending_index[position] - 1U];

			if (pending->used && pending->cluster == cluster && pending->sector == sector) {
				return true;
			}
			position = (position + 1U) & (proxy.pending_index_size - 1U);
		}
	}
	return false;
}


/**
 * @brief Forget the data held for one sector.
 *
 * @details Forget RAM data held for a sector: the host rewrote it elsewhere,
 * or freed the cluster. The entry stays in the array but is marked unused, so
 * the next compaction drops it; the index is left alone.
 *
 * @param[in] cluster Cluster the sector belongs to.
 * @param[in] sector  Sector offset inside the cluster.
 */
void proxy_pending_drop(uint32_t cluster, uint16_t sector)
{
	uint32_t position;

	if (proxy.pending_index_size == 0U) {
		return;
	}
	position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
	while (proxy.pending_index[position] != 0U) {
		struct proxy_pending_sector *pending = &proxy.pending[proxy.pending_index[position] - 1U];

		if (pending->cluster == cluster && pending->sector == sector) {
			pending->used = false;
			return;
		}
		position = (position + 1U) & (proxy.pending_index_size - 1U);
	}
}


/**
 * @brief Find the staging run that covers a cluster.
 *
 * @details A cluster belongs to a run from its first cluster up to its end
 * plus the configured gap, because the host may skip a few clusters.
 *
 * @param[in] cluster Cluster to look for.
 * @return The run, or NULL.
 */
static struct proxy_run *proxy_find_run(uint32_t cluster)
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
static uint32_t proxy_run_unclaimed(const struct proxy_run *run)
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
static struct proxy_run *proxy_new_run(uint32_t cluster)
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
static int proxy_migrate_pending(struct proxy_run *run, uint32_t start, uint32_t end)
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
static bool proxy_run_claimed(const struct proxy_run *run, uint32_t cluster)
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
static bool proxy_run_all_claimed(const struct proxy_run *run)
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
static int proxy_copy_run_range(struct proxy_run *run, const struct proxy_node *node, uint32_t low, uint32_t high)
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
static int proxy_claim_runs(uint32_t node_index)
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


/**
 * @brief Adopt the extent a host entry described for a node.
 *
 * @details The cluster range is validated against the view, then written to the
 * node; the allocation tail moves forward when the extent reaches past it, and
 * the cluster index is rebuilt. Staged data for the new extent is claimed first,
 * and only then are the waiting RAM sectors of the node flushed, so data that
 * was staged wins over data that is still in RAM.
 *
 * @param[in] node_index    Node index.
 * @param[in] first_cluster First cluster claimed by the entry.
 * @param[in] size          Data length claimed by the entry.
 * @return 0 on success, or a negative errno.
 */
int proxy_set_extent(uint32_t node_index, uint32_t first_cluster, uint64_t size)
{
	struct proxy_node *node = &proxy.nodes[node_index];
	int result;
	uint32_t count = size == 0U ? 0U : (uint32_t)((size + PROXY_CLUSTER_SIZE - 1U) / PROXY_CLUSTER_SIZE);

	if (count != 0U && (first_cluster < 2U || first_cluster + count > proxy.total_clusters + 2U)) {
		return -ENOSPC;
	}
	node->first_cluster = count == 0U ? 0U : first_cluster;
	node->cluster_count = count;
	if (count != 0U && first_cluster + count > proxy.next_cluster) {
		proxy.next_cluster = first_cluster + count;
	}
	proxy_rebuild_ranges();
	result = proxy_claim_runs(node_index);
	if (result != 0) {
		return result;
	}
	return proxy_pending_flush(node_index);
}


/**
 * @brief Add a node without giving it clusters yet.
 *
 * @details The layout-ready flag is lowered around @c proxy_add_node() so the
 * node is not allocated from the tail, then restored; the node keeps a zero
 * extent until the host entry supplies one.
 *
 * @param[in] parent      Parent node index.
 * @param[in] name        Requested name.
 * @param[in] directory   True for a directory.
 * @param[in] source      Where the node's data comes from.
 * @param[in] source_path Source path, or NULL.
 * @param[in] size        File size in bytes.
 * @return The new node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_add_unallocated(uint32_t parent, const char *name, bool directory,
	enum proxy_source source, const char *source_path, uint64_t size)
{
	bool ready = proxy.layout_ready;
	uint32_t index;

	proxy.layout_ready = false;
	index = proxy_add_node(parent, name, directory, source, source_path, size);
	proxy.layout_ready = ready;
	if (index != PROXY_INVALID_NODE) {
		proxy.nodes[index].first_cluster = 0U;
		proxy.nodes[index].cluster_count = 0U;
	}
	return index;
}


/**
 * @brief Carry out the deletes the host requested and sync the source mounts.
 *
 * @details First every live source file is opened for writing: a missing file
 * means it was deleted behind the view's back and the node is marked deleted,
 * and an existing one is truncated to the node's size, which removes bytes the
 * host discarded. Then the deleted nodes are removed in reverse index order, so
 * children go before their parents; a directory that is not empty is kept and
 * retried at the next flush instead of failing the host. Finally every mounted
 * source volume is synced. A lost source fails the whole pass immediately.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_apply_deletes(void)
{
	int result = 0;

	if (proxy.source_lost) {
		return -ENODEV;
	}

	for (uint32_t index = 1U; result == 0 && index < proxy.node_count; index++) {
		struct proxy_node *node = &proxy.nodes[index];

		if (!node->deleted && !node->directory && node->source == PROXY_SOURCE_VFS) {
			int descriptor = vfs_open(proxy_string(node->source_offset), VFS_O_WRITE);

			if (descriptor == -ENOENT) {
				node->deleted = true;
				continue;
			}
			if (descriptor < 0) {
				result = descriptor;
			} else {
				result = vfs_ftruncate(descriptor, node->size);
				(void)vfs_close(descriptor);
			}
		}
	}

	for (uint32_t index = proxy.node_count; result == 0 && index-- > 1U;) {
		struct proxy_node *node = &proxy.nodes[index];

		if (!node->deleted || node->source != PROXY_SOURCE_VFS) {
			continue;
		}
		result = node->directory ? vfs_rmdir(proxy_string(node->source_offset)) :
			vfs_unlink(proxy_string(node->source_offset));
		if (result == -ENOENT) {
			result = 0;
		} else if (result == -ENOTEMPTY) {
			/* Something is left in it. Keep the directory and try again at the next flush, rather than fail the host. */
			result = 0;
		} else if (result != 0) {
			return result;
		}
	}
	for (uint32_t partition = 0U; result == 0 && partition < PROXY_PARTITIONS; partition++) {
		if (proxy.partitions[partition].mounted) {
			result = vfs_sync_mount(proxy.partitions[partition].mount);
		}
	}
	return result;
}
