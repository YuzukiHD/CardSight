// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_write_data.c
 * @brief File data writes and claiming staged clusters.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/**
 * @brief Fill a source file with zeros up to a position before writing there.
 *
 * @details Some file systems cannot seek past the end of a file, and a seek in
 * write mode may extend it without clearing the new range. Data may arrive out
 * of order, so the gap is written explicitly.
 *
 * @param[in] descriptor Open descriptor of the file.
 * @param[in] position   Position the following write will use.
 * @return 0 on success, or a negative errno.
 */
int lazy_fill_to(int descriptor, uint64_t position)
{
	struct vfs_stat status;
	uint8_t zeros[PROXY_LAZY_SECTOR_SIZE];
	uint64_t size;

	if (vfs_fstat(descriptor, &status) != 0) {
		return 0; /* the seek that follows reports what is wrong */
	}
	size = status.size;
	if (size >= position) {
		return 0;
	}
	if (vfs_lseek(descriptor, (int64_t)size, 0) < 0) {
		return -EIO;
	}
	memset(zeros, 0, sizeof(zeros));
	while (size < position) {
		size_t length = position - size < sizeof(zeros) ?
			(size_t)(position - size) : sizeof(zeros);
		int written = vfs_write(descriptor, zeros, length);

		if (written <= 0) {
			return written < 0 ? written : -EIO;
		}
		size += (uint64_t)written;
	}
	return 0;
}

/**
 * @brief Write bytes of a view file to its source file.
 *
 * @details One write descriptor is kept open and reused while the host writes
 * the same file. A missing source file is created; a gap before the write is
 * filled with zeros first. The cached read descriptor of the same file is
 * dropped so that a following read sees the new bytes.
 *
 * @param[in] dir_id      Directory that owns the file.
 * @param[in] entry_index Cached entry index.
 * @param[in] offset      Byte offset inside the file.
 * @param[in] data        Bytes to write.
 * @param[in] length      Number of bytes.
 * @return 0 on success, or a negative errno.
 */
int lazy_write_bytes(uint32_t dir_id, uint32_t entry_index, uint64_t offset,
	const uint8_t *data, size_t length)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dirent *item = &dir->entries[entry_index];
	char path[VFS_PATH_MAX];
	size_t done = 0U;
	int result;

	if (item->deleted || item->directory) {
		return -EIO;
	}
	result = lazy_entry_path(dir_id, item, path, sizeof(path));
	if (result != 0) {
		return result;
	}
	if (lazy.cached_fd >= 0 && lazy.cached_dir == dir_id &&
	    lazy.cached_slot == item->slot) {
		(void)vfs_close(lazy.cached_fd);
		lazy.cached_fd = -1;
		lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	}
	if (lazy.write_fd < 0 || lazy.write_dir != dir_id || lazy.write_slot != item->slot) {
		lazy_write_cache_drop();
		lazy.write_fd = vfs_open(path, VFS_O_READ | VFS_O_WRITE);
		if (lazy.write_fd < 0) {
			lazy.write_fd = vfs_open(path,
				VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE);
		}
		if (lazy.write_fd < 0) {
			lazy.write_dir = PROXY_LAZY_INVALID_DIR;
			return lazy.write_fd;
		}
		lazy.write_dir = dir_id;
		lazy.write_slot = item->slot;
		lazy.write_position = 0U;
	}
	result = lazy_fill_to(lazy.write_fd, offset);
	if (result == 0 && lazy.write_position != offset) {
		int64_t position = vfs_lseek(lazy.write_fd, (int64_t)offset, 0);

		if (position < 0) {
			result = (int)position;
		} else {
			lazy.write_position = offset;
		}
	}
	while (result == 0 && done < length) {
		int written = vfs_write(lazy.write_fd, data + done, length - done);

		if (written <= 0) {
			result = written < 0 ? written : -EIO;
		} else {
			done += (size_t)written;
			lazy.write_position += (size_t)written;
		}
	}
	if (result != 0) {
		lazy_write_cache_drop();
		lazy.last_error = result;
		return result;
	}
	lazy.file_bytes_written += done;
	lazy.clean = false;
	lazy.dirs[dir_id].sizes_dirty = true;
	return 0;
}

/**
 * @brief Write the pending sectors that now resolve to one directory's files.
 *
 * @param[in] dir_id Directory whose ranges were just rebuilt.
 * @return 0 on success, or a negative errno.
 */
int lazy_claim_pending_dir(uint32_t dir_id)
{
	uint32_t kept = 0U;

	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		struct proxy_lazy_pending_sector *pending = &lazy.pending[index];
		struct proxy_lazy_range range;
		uint32_t entry_index;

		if (!pending->used) {
			continue;
		}
		if (lazy_range_find(pending->cluster, &range) != 0 ||
		    range.kind != PROXY_LAZY_RANGE_DATA || range.dir_id != dir_id ||
		    range.slot == PROXY_LAZY_INVALID_SLOT) {
			if (kept != index) {
				lazy.pending[kept] = *pending;
			}
			kept++;
			continue;
		}
		entry_index = lazy_entry_by_slot(&lazy.dirs[dir_id], range.slot);
		if (entry_index == UINT32_MAX ||
		    lazy.dirs[dir_id].entries[entry_index].deleted) {
			if (kept != index) {
				lazy.pending[kept] = *pending;
			}
			kept++;
			continue;
		}
		{
			uint64_t offset = ((uint64_t)range.offset +
				(pending->cluster - range.base)) * PROXY_LAZY_CLUSTER_SIZE +
				(uint64_t)pending->sector * PROXY_LAZY_SECTOR_SIZE;
			int result = lazy_write_bytes(dir_id, entry_index, offset,
				pending->data, PROXY_LAZY_SECTOR_SIZE);

			if (result != 0) {
				return result;
			}
			pending->used = false;
		}
	}
	lazy.pending_count = kept;
	return 0;
}

/**
 * @brief Copy staged clusters whose directory entries have appeared.
 *
 * @details Each written cluster of a staging run is looked up in the range
 * index. A file range takes the staged sector at its file offset, capped by the
 * source valid length; a directory range feeds the sector back through the
 * directory writer. Clusters with no range yet stay staged, and a run whose
 * every written cluster was claimed or freed is released and its file unlinked.
 * A guard keeps a directory write triggered by a claim from recursing.
 *
 * @return 0 on success, or a negative errno that stopped the copy.
 */
int lazy_claim_runs(void)
{
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE];
	int result = 0;

	if (lazy.runs == NULL || lazy.claiming) {
		return 0;
	}
	lazy.claiming = true;
	for (uint32_t run_index = 0U;
	     run_index < CONFIG_STORAGE_PROXY_LAZY_RUNS && result == 0; run_index++) {
		struct proxy_lazy_run *run = &lazy.runs[run_index];
		bool done = true;

		if (!run->used) {
			continue;
		}
		for (uint32_t offset = 0U; offset < run->clusters && result == 0; offset++) {
			uint32_t cluster = run->first_cluster + offset;
			struct proxy_lazy_range range;

			if (lazy_run_bit(run->claimed, offset)) {
				continue;
			}
			if (lazy_run_bit(run->dropped, offset) ||
			    !lazy_run_bit(run->written, offset)) {
				lazy_run_set(run->claimed, offset);
				continue;
			}
			if (lazy_range_find(cluster, &range) != 0) {
				done = false;
				continue;
			}
			if (range.kind == PROXY_LAZY_RANGE_DATA) {
				uint32_t entry_index;
				uint64_t file_offset;
				uint32_t sectors;

				if (!lazy.dirs[range.dir_id].cache_ready) {
					result = lazy_dir_load(range.dir_id);
					if (result != 0) {
						break;
					}
				}
				entry_index = lazy_entry_by_slot(&lazy.dirs[range.dir_id], range.slot);
				if (entry_index == UINT32_MAX ||
				    lazy.dirs[range.dir_id].entries[entry_index].deleted) {
					done = false;
					continue;
				}
				{
					const struct proxy_lazy_dirent *item =
						&lazy.dirs[range.dir_id].entries[entry_index];

					file_offset = ((uint64_t)range.offset +
						(cluster - range.base)) * PROXY_LAZY_CLUSTER_SIZE;
					if (item->directory || file_offset >= item->size) {
						lazy_run_set(run->claimed, offset);
						continue;
					}
					sectors = PROXY_LAZY_SECTORS_PER_CLUSTER;
					if (item->size - file_offset <
					    (uint64_t)sectors * PROXY_LAZY_SECTOR_SIZE) {
						sectors = (uint32_t)((item->size - file_offset +
							PROXY_LAZY_SECTOR_SIZE - 1U) /
							PROXY_LAZY_SECTOR_SIZE);
					}
				}
				for (uint32_t sector_index = 0U;
				     sector_index < sectors && result == 0; sector_index++) {
					uint64_t staged = ((uint64_t)offset *
						PROXY_LAZY_SECTORS_PER_CLUSTER + sector_index) *
						PROXY_LAZY_SECTOR_SIZE;

					result = lazy_run_read(run, staged, sector);
					if (result == 0) {
						result = lazy_write_bytes(range.dir_id, entry_index,
							file_offset + (uint64_t)sector_index *
							PROXY_LAZY_SECTOR_SIZE,
							sector, PROXY_LAZY_SECTOR_SIZE);
					}
				}
				if (result == 0) {
					lazy_run_set(run->claimed, offset);
				}
			} else if (range.kind == PROXY_LAZY_RANGE_DIRECTORY) {
				for (uint32_t sector_index = 0U;
				     sector_index < PROXY_LAZY_SECTORS_PER_CLUSTER &&
				     result == 0; sector_index++) {
					uint64_t staged = ((uint64_t)offset *
						PROXY_LAZY_SECTORS_PER_CLUSTER + sector_index) *
						PROXY_LAZY_SECTOR_SIZE;
					uint64_t directory_offset = ((uint64_t)range.offset +
						(cluster - range.base)) * PROXY_LAZY_CLUSTER_SIZE +
						(uint64_t)sector_index * PROXY_LAZY_SECTOR_SIZE;

					result = lazy_run_read(run, staged, sector);
					if (result == 0) {
						result = lazy_write_directory(range.dir_id,
							directory_offset, sector);
					}
				}
				if (result == 0) {
					lazy_run_set(run->claimed, offset);
				}
			} else {
				done = false;
			}
		}
		if (result != 0) {
			break;
		}
		for (uint32_t offset = 0U; offset < run->clusters; offset++) {
			if (!lazy_run_bit(run->claimed, offset) &&
			    lazy_run_bit(run->written, offset) &&
			    !lazy_run_bit(run->dropped, offset)) {
				done = false;
				break;
			}
		}
		if (done) {
			lazy_run_release(run);
		}
	}
	lazy.claiming = false;
	if (result != 0) {
		lazy.last_error = result;
	}
	return result;
}
