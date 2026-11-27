// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_staging.c
 * @brief Sparse staging files for host data that outlives the pending table.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Read one bit of a run's per-cluster bitmap.
 *
 * @param[in] bits  Bitmap.
 * @param[in] index Cluster index inside the run.
 * @return True when the bit is set.
 */
bool lazy_run_bit(const uint8_t *bits, uint32_t index)
{
	return (bits[index / 8U] & (uint8_t)(1U << (index % 8U))) != 0U;
}

/**
 * @brief Set one bit of a run's per-cluster bitmap.
 *
 * @param[in,out] bits  Bitmap.
 * @param[in]     index Cluster index inside the run.
 */
void lazy_run_set(uint8_t *bits, uint32_t index)
{
	bits[index / 8U] |= (uint8_t)(1U << (index % 8U));
}

/**
 * @brief Clear one bit of a run's per-cluster bitmap.
 *
 * @param[in,out] bits  Bitmap.
 * @param[in]     index Cluster index inside the run.
 */
void lazy_run_clear(uint8_t *bits, uint32_t index)
{
	bits[index / 8U] &= (uint8_t)~(1U << (index % 8U));
}

/**
 * @brief Find the staging run that maps a cluster.
 *
 * @param[in] cluster View cluster.
 * @return The run, or NULL.
 */
struct proxy_lazy_run *lazy_run_find(uint32_t cluster)
{
	if (lazy.runs == NULL) {
		return NULL;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
		struct proxy_lazy_run *run = &lazy.runs[index];

		if (!run->used || cluster < run->first_cluster) {
			continue;
		}
		if (cluster - run->first_cluster < run->clusters) {
			return run;
		}
	}
	return NULL;
}

/**
 * @brief Find a run that may absorb a cluster just past its span.
 *
 * @details The host may skip a few clusters between two writes while it
 * allocates ahead, so a small gap is folded into the existing run instead of
 * starting another staging file.
 *
 * @param[in] cluster View cluster.
 * @return The run with the closest end, or NULL.
 */
struct proxy_lazy_run *lazy_run_extendable(uint32_t cluster)
{
	struct proxy_lazy_run *best = NULL;

	if (lazy.runs == NULL) {
		return NULL;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
		struct proxy_lazy_run *run = &lazy.runs[index];
		uint32_t end;

		if (!run->used || cluster < run->first_cluster) {
			continue;
		}
		end = run->first_cluster + run->clusters;
		if (cluster < end || cluster - end > CONFIG_STORAGE_PROXY_LAZY_RUN_GAP) {
			continue;
		}
		if (cluster - run->first_cluster >= CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS) {
			continue;
		}
		if (best == NULL || end > best->first_cluster + best->clusters) {
			best = run;
		}
	}
	return best;
}

/**
 * @brief Release one staging run and unlink its file.
 *
 * @param[in,out] run Run to release.
 */
void lazy_run_release(struct proxy_lazy_run *run)
{
	if (!run->used) {
		return;
	}
	if (run->fd >= 0) {
		(void)vfs_close(run->fd);
		run->fd = -1;
	}
	(void)vfs_unlink(run->path);
	run->used = false;
	run->clusters = 0U;
	run->position = 0U;
}

/**
 * @brief Release every staging run.
 */
void lazy_run_release_all(void)
{
	if (lazy.runs == NULL) {
		return;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
		lazy_run_release(&lazy.runs[index]);
	}
	lazy.staged_sectors = 0U;
}

/**
 * @brief Open a staging file for a stream that begins at a cluster.
 *
 * @details Every selected, writable source partition can host the file; the
 * first one is used. When every slot is busy a claim pass runs first, because
 * an entry that arrived meanwhile may have retired a run.
 *
 * @param[in] first First view cluster of the stream.
 * @return The run, or NULL when no writable source or no slot is available.
 */
struct proxy_lazy_run *lazy_run_create(uint32_t first)
{
	struct proxy_lazy_run *run = NULL;
	uint64_t best_free = 0U;
	uint32_t partition = PROXY_LAZY_PARTITIONS;

	if (lazy.runs == NULL) {
		return NULL;
	}
	/* The tail is backed by every source, so stage where the most room is. */
	for (uint32_t index = 0U; index < PROXY_LAZY_PARTITIONS; index++) {
		struct proxy_lazy_partition *part = &lazy.partitions[index];

		if (part->selected && part->mounted && !part->volume.read_only &&
		    (partition >= PROXY_LAZY_PARTITIONS || part->volume.free_bytes > best_free)) {
			partition = index;
			best_free = part->volume.free_bytes;
		}
	}
	if (partition >= PROXY_LAZY_PARTITIONS) {
		return NULL;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
		if (!lazy.runs[index].used) {
			run = &lazy.runs[index];
			break;
		}
	}
	if (run == NULL) {
		(void)lazy_claim_runs();
		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
			if (!lazy.runs[index].used) {
				run = &lazy.runs[index];
				break;
			}
		}
	}
	if (run == NULL) {
		return NULL;
	}
	snprintf(run->path, sizeof(run->path), "%s/" PROXY_LAZY_STAGE_PREFIX "%u",
		lazy.partitions[partition].mount, first);
	(void)vfs_unlink(run->path);
	run->fd = vfs_open(run->path, VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
	if (run->fd < 0) {
		return NULL;
	}
	run->used = true;
	run->partition = partition;
	run->first_cluster = first;
	run->clusters = 0U;
	run->position = 0U;
	memset(run->written, 0, sizeof(run->written));
	memset(run->claimed, 0, sizeof(run->claimed));
	memset(run->dropped, 0, sizeof(run->dropped));
	lazy.clean = false;
	return run;
}

/**
 * @brief Write one host sector into a staging run.
 *
 * @details The sector lands at its view offset, so a sparse stretch of the
 * stream costs no card space. New data clears the claimed bit: the cluster must
 * be copied again after a later claim.
 *
 * @param[in,out] run     Run receiving the sector.
 * @param[in]     cluster View cluster.
 * @param[in]     sector  Sector offset inside the cluster.
 * @param[in]     data    Sector contents.
 * @return 0 on success, or a negative errno.
 */
int lazy_run_write(struct proxy_lazy_run *run, uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	uint32_t index;
	uint64_t offset;
	size_t done = 0U;
	int result = 0;

	if (cluster < run->first_cluster) {
		return -ENOSPC;
	}
	index = cluster - run->first_cluster;
	if (index >= CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS) {
		return -ENOSPC;
	}
	offset = ((uint64_t)index * PROXY_LAZY_SECTORS_PER_CLUSTER + sector) *
		PROXY_LAZY_SECTOR_SIZE;
	if (run->position != offset) {
		int64_t position = vfs_lseek(run->fd, (int64_t)offset, 0);

		if (position < 0) {
			return (int)position;
		}
		run->position = offset;
	}
	while (done < PROXY_LAZY_SECTOR_SIZE) {
		int written = vfs_write(run->fd, data + done, PROXY_LAZY_SECTOR_SIZE - done);

		if (written <= 0) {
			result = written < 0 ? written : -EIO;
			break;
		}
		done += (size_t)written;
		run->position += (uint64_t)written;
	}
	if (result != 0) {
		return result;
	}
	lazy_run_set(run->written, index);
	lazy_run_clear(run->dropped, index);
	lazy_run_clear(run->claimed, index);
	if (index + 1U > run->clusters) {
		run->clusters = index + 1U;
	}
	lazy.staged_sectors++;
	lazy.clean = false;
	return 0;
}

/**
 * @brief Read one staged sector back from a run.
 *
 * @details Reading moves the descriptor, so the cached write position is
 * invalidated and the next append seeks explicitly.
 *
 * @param[in,out] run    Run to read.
 * @param[in]     offset Byte offset inside the staging file.
 * @param[out]    data   Destination for one sector.
 * @return 0 on success, or a negative errno.
 */
int lazy_run_read(struct proxy_lazy_run *run, uint64_t offset,
	uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	size_t done = 0U;
	int64_t position = vfs_lseek(run->fd, (int64_t)offset, 0);

	if (position < 0) {
		return (int)position;
	}
	run->position = UINT64_MAX;
	while (done < PROXY_LAZY_SECTOR_SIZE) {
		int got = vfs_read(run->fd, data + done, PROXY_LAZY_SECTOR_SIZE - done);

		if (got <= 0) {
			return got < 0 ? got : -EIO;
		}
		done += (size_t)got;
	}
	return 0;
}

/**
 * @brief Forget staged data of a cluster the host freed.
 *
 * @details The host may reuse the cluster for another file; staged bytes from
 * the old stream must not reach the new owner.
 *
 * @param[in] cluster Cluster that became free.
 */
void lazy_run_drop_cluster(uint32_t cluster)
{
	if (lazy.runs == NULL) {
		return;
	}
	for (uint32_t run_index = 0U; run_index < CONFIG_STORAGE_PROXY_LAZY_RUNS; run_index++) {
		struct proxy_lazy_run *run = &lazy.runs[run_index];
		uint32_t index;

		if (!run->used || cluster < run->first_cluster) {
			continue;
		}
		index = cluster - run->first_cluster;
		if (index >= run->clusters) {
			continue;
		}
		lazy_run_set(run->dropped, index);
		lazy_run_clear(run->written, index);
		lazy_run_clear(run->claimed, index);
	}
}
