// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_read.c
 * @brief File reads, cached descriptors and sector reads of the view.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- file data ---- */

/**
 * @brief Close the cached write descriptor, flushing the file.
 */
void lazy_write_cache_drop(void)
{
	if (lazy.write_fd >= 0) {
		(void)vfs_close(lazy.write_fd);
		lazy.write_fd = -1;
	}
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.write_position = 0U;
}

/**
 * @brief Read one sector of a view file from its source file.
 *
 * @details The sector is zeroed first, so a read past the end returns zeros.
 * One source descriptor is kept open and reused while the host reads the same
 * file; a different file closes and reopens it.
 *
 * @param[in]  dir_id      Directory that owns the file.
 * @param[in]  entry_index Cached entry index.
 * @param[in]  offset      Byte offset inside the file.
 * @param[out] sector      Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
int lazy_read_file(uint32_t dir_id, uint32_t entry_index, uint64_t offset,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	const struct proxy_lazy_dirent *item = &dir->entries[entry_index];
	char path[VFS_PATH_MAX];
	size_t length;
	size_t done = 0U;
	int result;

	memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
	if (item->deleted || offset >= item->size) {
		return 0;
	}
	/* A read must see what the host just wrote through the write descriptor. */
	if (lazy.write_fd >= 0 && lazy.write_dir == dir_id && lazy.write_slot == item->slot) {
		lazy_write_cache_drop();
	}
	length = item->size - offset < PROXY_LAZY_SECTOR_SIZE ?
		(size_t)(item->size - offset) : PROXY_LAZY_SECTOR_SIZE;
	result = lazy_entry_path(dir_id, item, path, sizeof(path));
	if (result != 0) {
		return result;
	}
	if (lazy.cached_fd < 0 || lazy.cached_dir != dir_id || lazy.cached_slot != item->slot) {
		if (lazy.cached_fd >= 0) {
			(void)vfs_close(lazy.cached_fd);
		}
		lazy.cached_fd = vfs_open(path, VFS_O_READ);
		if (lazy.cached_fd < 0) {
			lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
			return lazy.cached_fd;
		}
		lazy.cached_dir = dir_id;
		lazy.cached_slot = item->slot;
		lazy.cached_position = 0U;
	}
	if (lazy.cached_position != offset) {
		int64_t position = vfs_lseek(lazy.cached_fd, (int64_t)offset, 0);

		if (position < 0) {
			return (int)position;
		}
		lazy.cached_position = offset;
	}
	while (done < length) {
		result = vfs_read(lazy.cached_fd, sector + done, length - done);
		if (result <= 0) {
			return result == 0 ? -EIO : result;
		}
		done += (size_t)result;
		lazy.cached_position += (size_t)result;
	}
	lazy.file_bytes += done;
	return 0;
}

/* ---- sector reads ---- */

/**
 * @brief Read one sector of the view by its sector number.
 *
 * @details Sectors before the FAT are boot records; the FAT is generated entry
 * by entry. A data sector is resolved to the bitmap, the up-case table, a
 * directory extent or a file-data block. A cluster inside the reservation that
 * no loaded object owns reads as zeros.
 *
 * @param[in]  sector_number Logical sector number inside the view.
 * @param[out] sector        Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
int lazy_read_sector(uint64_t sector_number, uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	uint64_t relative;
	uint32_t cluster;
	uint32_t sector_in_cluster;
	struct proxy_lazy_range range;

	if (sector_number >= lazy.total_sectors) {
		return -EINVAL;
	}
	if (sector_number < PROXY_LAZY_BOOT_SECTORS) {
		uint32_t record = (uint32_t)(sector_number % 12U);

		if (record == 11U) {
			uint32_t checksum = lazy_boot_checksum();

			for (uint32_t index = 0U; index < PROXY_LAZY_SECTOR_SIZE; index += 4U) {
				lazy_put_le32(sector + index, checksum);
			}
		} else {
			lazy_boot_record(record, sector);
		}
		return 0;
	}
	if (sector_number < PROXY_LAZY_FAT_OFFSET) {
		memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
		return 0;
	}
	if (sector_number < PROXY_LAZY_FAT_OFFSET + lazy.fat_sectors) {
		uint32_t fat_sector = (uint32_t)(sector_number - PROXY_LAZY_FAT_OFFSET);
		uint32_t first = fat_sector * (PROXY_LAZY_SECTOR_SIZE / 4U);

		for (uint32_t index = 0U; index < PROXY_LAZY_SECTOR_SIZE / 4U; index++) {
			lazy_put_le32(sector + index * 4U, lazy_fat_entry(first + index));
		}
		return 0;
	}
	if (sector_number < lazy.data_start) {
		memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
		return 0;
	}
	relative = sector_number - lazy.data_start;
	cluster = (uint32_t)(relative / PROXY_LAZY_SECTORS_PER_CLUSTER) +
		PROXY_LAZY_ROOT_CLUSTER_MIN;
	sector_in_cluster = (uint32_t)(relative % PROXY_LAZY_SECTORS_PER_CLUSTER);
	memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
	if (cluster >= lazy.bitmap_cluster && cluster < lazy.bitmap_cluster + lazy.bitmap_clusters) {
		uint64_t bitmap_offset = (uint64_t)(cluster - lazy.bitmap_cluster) *
			PROXY_LAZY_CLUSTER_SIZE + (uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;
		size_t length = bitmap_offset < lazy.bitmap_bytes ?
			lazy.bitmap_bytes - (size_t)bitmap_offset : 0U;

		if (length > PROXY_LAZY_SECTOR_SIZE) {
			length = PROXY_LAZY_SECTOR_SIZE;
		}
		if (length != 0U) {
			memcpy(sector, lazy.bitmap + bitmap_offset, length);
		}
		return 0;
	}
	if (cluster >= lazy.upcase_cluster && cluster < lazy.upcase_cluster + lazy.upcase_clusters) {
		uint64_t upcase_offset = (uint64_t)(cluster - lazy.upcase_cluster) *
			PROXY_LAZY_CLUSTER_SIZE + (uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;
		size_t length = upcase_offset < lazy.upcase_bytes ?
			lazy.upcase_bytes - (size_t)upcase_offset : 0U;

		if (length > PROXY_LAZY_SECTOR_SIZE) {
			length = PROXY_LAZY_SECTOR_SIZE;
		}
		if (length != 0U) {
			memcpy(sector, lazy.upcase + upcase_offset, length);
		}
		return 0;
	}
	if (lazy_root_text_read(cluster, sector_in_cluster, sector) != 0) {
		return 0;
	}
	if (lazy_range_find(cluster, &range) != 0) {
		return 0;
	}
	if (range.kind == PROXY_LAZY_RANGE_DIRECTORY) {
		uint64_t offset = ((uint64_t)range.offset + (cluster - range.base)) *
			PROXY_LAZY_CLUSTER_SIZE +
			(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;

		return lazy_render_directory(range.dir_id, offset, sector);
	}
	{
		int result = lazy_dir_load(range.dir_id);
		uint32_t index;

		if (result != 0) {
			return result;
		}
		if (range.slot == PROXY_LAZY_INVALID_SLOT) {
			return 0;
		}
		index = lazy_entry_by_slot(&lazy.dirs[range.dir_id], range.slot);
		if (index == UINT32_MAX) {
			return 0;
		}
		return lazy_read_file(range.dir_id, index,
			((uint64_t)range.offset + (cluster - range.base)) *
			PROXY_LAZY_CLUSTER_SIZE +
			(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE, sector);
	}
}

/**
 * @brief Block device read callback of the lazy view.
 *
 * @param[in]  disk   Disk being read; unused.
 * @param[in]  sector First logical sector.
 * @param[out] buffer Buffer receiving the data.
 * @param[in]  count  Number of sectors to read.
 * @return 0 on success, or a negative errno.
 */
int lazy_disk_read(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count)
{
	uint8_t *output = buffer;
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
	if (sector > lazy.total_sectors || (uint64_t)count > lazy.total_sectors - sector) {
		mutex_unlock(&lazy.lock);
		lazy_armed_leave();
		return -EINVAL;
	}
	for (uint32_t index = 0U; index < count; index++) {
		result = lazy_read_sector(sector + index, output + (size_t)index * PROXY_LAZY_SECTOR_SIZE);
		if (result != 0) {
			break;
		}
	}
	lazy.sectors_read += count;
	if (result != 0) {
		pr_warn("lazy proxy: read at sector %llu failed: %d\n",
			(unsigned long long)sector, result);
	}
	mutex_unlock(&lazy.lock);
	lazy_armed_leave();
	return result;
}
