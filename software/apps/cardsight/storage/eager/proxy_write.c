// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_write.c
 * @brief Host data writes, staging flushes, extents and deferred deletes.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/**
 * @brief Report whether a path is the staging file of an open run.
 *
 * @details The comparison is by pointer, because the path buffer of a run is
 * what the batch queue holds while it points at a staging file.
 *
 * @param[in] path Path to test.
 * @return True when @p path is one of the run paths.
 */
bool proxy_is_stage_path(const char *path)
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
int proxy_batch_write(const char *path, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE])
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
