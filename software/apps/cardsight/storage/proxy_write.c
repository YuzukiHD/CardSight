// SPDX-License-Identifier: GPL-2.0-or-later
/* Host writes: data that arrives before its entry, staging on the card, claims and deletes. */

#include "proxy_internal.h"


static bool proxy_is_stage_path(const char *path)
{
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		if (path == proxy.runs[index].path) {
			return true;
		}
	}
	return false;
}


void proxy_stage_close(void)
{
	if (proxy.stage_fd >= 0) {
		(void)vfs_close(proxy.stage_fd);
		proxy.stage_fd = -1;
	}
	proxy.stage_path = NULL;
}


/* Write the waiting run to its file with one open, one seek and as few writes as the buffer allows. */
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


/* Queue a sector for a file; consecutive sectors of one file leave as a single write. */
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


static uint32_t proxy_pending_hash(uint32_t cluster, uint16_t sector)
{
	return ((cluster * 64U + sector) * 2654435761U) >> 7;
}


/* Rebuild the index from the array; after a flush compacts it or the array grows. */
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


static int proxy_pending_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE])
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_pending_store_inner(cluster, sector, data);

	proxy_time_end(&proxy_stats.pending_store, begin);
	return result;
}


/* Sectors that belong to the file are written in order, the rest stay for another entry. */
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


static int proxy_pending_flush(uint32_t node_index)
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_pending_flush_inner(node_index);
	proxy_time_end(&proxy_stats.pending_flush, begin);
	return result;
}


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


/* Forget RAM data held for a sector: the host rewrote it elsewhere, or freed the cluster. */
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


/* Clusters of a run that still wait for an entry. A tail cluster that ends mid-way is never complete. */
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


/* Move the RAM data of clusters [start, end) into the run's file; it is the same stream continuing. */
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


/*
 * A host writes file data before the entry that owns it, often megabytes of it. Small amounts wait in
 * RAM; beyond that the sectors go to a staging file on the source card until an entry claims them.
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


static bool proxy_run_claimed(const struct proxy_run *run, uint32_t cluster)
{
	uint32_t index = cluster - run->first_cluster;

	return index < PROXY_RUN_TRACKED && (run->claimed[index / 8U] & (1U << (index % 8U))) != 0U;
}


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


/* Copy clusters [low, high) of a run into the file that owns them; returns what is wrong, if anything. */
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


/* The directory entry arrived: its clusters take the staged data, by rename when it is all of the run. */
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
