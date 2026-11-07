// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_io.c
 * @brief Block device operations of the view, and the translation of a sector to its source data.
 *
 * A read is resolved from the on-disk structure of the view: boot region,
 * FAT, bitmap, up-case table, then data clusters, which are mapped back to a
 * node and then to the source file. A write is routed in the same way, except
 * that a data cluster with no owner is staged until its directory entry
 * arrives. Every callback takes @c proxy.lock.
 */

#include "proxy_internal.h"


/**
 * @brief Position the cached read descriptor on a node and offset.
 *
 * @details The descriptor is reused only for the same node; a different node
 * closes it and opens the source file again. A seek is issued only when the
 * cached position differs from the requested offset.
 *
 * @param[in] node_index Node to read from.
 * @param[in] offset     Byte offset to seek to.
 * @return 0 on success, or a negative errno.
 */
static int proxy_open_cached(uint32_t node_index, uint64_t offset)
{
	const struct proxy_node *node = &proxy.nodes[node_index];

	if (proxy.cached_node != node_index) {
		if (proxy.cached_fd >= 0) {
			vfs_close(proxy.cached_fd);
		}
		proxy.cached_fd = vfs_open(proxy_string(node->source_offset), VFS_O_READ);
		if (proxy.cached_fd < 0) {
			proxy.cached_node = PROXY_INVALID_NODE;
			return proxy.cached_fd;
		}
		proxy.cached_node = node_index;
		proxy.cached_position = 0U;
	}
	if (proxy.cached_position != offset) {
		int64_t result = vfs_lseek(proxy.cached_fd, offset, 0);

		if (result < 0) {
			return (int)result;
		}
		proxy.cached_position = offset;
	}
	return 0;
}


/**
 * @brief Read one sector of a file node.
 *
 * @details The sector is zeroed first; a read past the end therefore returns
 * zeros. Memory-backed nodes are copied straight from the arena, volatile ones
 * read as zeros, and source files are read through the cached descriptor in a
 * loop that tolerates short reads.
 *
 * @param[in]  node_index Node index.
 * @param[in]  offset     Byte offset of the sector inside the file.
 * @param[out] sector     Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
static int proxy_read_file(uint32_t node_index, uint64_t offset,
			   uint8_t sector[PROXY_SECTOR_SIZE])
{
	const struct proxy_node *node = &proxy.nodes[node_index];
	size_t length;

	memset(sector, 0, PROXY_SECTOR_SIZE);
	if (offset >= node->size) {
		return 0;
	}
	length = node->size - offset < PROXY_SECTOR_SIZE ?
		(size_t)(node->size - offset) : PROXY_SECTOR_SIZE;
	if (node->source == PROXY_SOURCE_MEMORY) {
		memcpy(sector, proxy_string(node->source_offset) + offset, length);
		return 0;
	}
	if (node->source == PROXY_SOURCE_VOLATILE) {
		return 0;
	}
	if (proxy_open_cached(node_index, offset) != 0) {
		return -EIO;
	}
	for (size_t done = 0U; done < length;) {
		int result = vfs_read(proxy.cached_fd, sector + done, length - done);

		if (result <= 0) {
			return result == 0 ? -EIO : result;
		}
		done += (size_t)result;
		proxy.cached_position += (size_t)result;
	}
	return 0;
}


/**
 * @brief Read one sector of the view by its sector number.
 *
 * @details Sectors before the FAT are boot records: record 11 is the checksum
 * sector and the rest come from @c proxy_boot_record(). The FAT is generated
 * entry by entry, the reserved gap reads as zeros, and a data sector is
 * resolved to the bitmap, the up-case table or a node. In the last case the
 * offset inside the node is computed and the directory or file reader is
 * called. A sector with no owner reads as zeros.
 *
 * @param[in]  sector_number Logical sector number inside the view.
 * @param[out] sector        Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
static int proxy_read_sector(uint64_t sector_number, uint8_t sector[PROXY_SECTOR_SIZE])
{
	if (proxy.source_lost) {
		return -ENODEV;
	}
	if (sector_number >= proxy.total_sectors) {
		return -EINVAL;
	}
	if (sector_number < PROXY_BOOT_SECTORS) {
		uint32_t record = (uint32_t)(sector_number % 12U);

		if (record == 11U) {
			uint32_t checksum = proxy_boot_checksum();

			for (uint32_t index = 0U; index < PROXY_SECTOR_SIZE; index += 4U) {
				put_le32(sector + index, checksum);
			}
		} else {
			proxy_boot_record(record, sector);
		}
		return 0;
	}
	if (sector_number < PROXY_FAT_OFFSET) {
		memset(sector, 0, PROXY_SECTOR_SIZE);
		return 0;
	}
	if (sector_number < PROXY_FAT_OFFSET + proxy.fat_sectors) {
		uint32_t fat_sector = (uint32_t)(sector_number - PROXY_FAT_OFFSET);
		uint32_t first = fat_sector * (PROXY_SECTOR_SIZE / 4U);

		for (uint32_t index = 0U; index < PROXY_SECTOR_SIZE / 4U; index++) {
			put_le32(sector + index * 4U, proxy_fat_entry(first + index));
		}
		return 0;
	}
	if (sector_number < proxy.data_start) {
		memset(sector, 0, PROXY_SECTOR_SIZE);
		return 0;
	}
	{
		uint64_t relative = sector_number - proxy.data_start;
		uint32_t cluster = (uint32_t)(relative / PROXY_SECTORS_PER_CLUSTER) + 2U;
		uint64_t cluster_offset = ((uint64_t)(cluster - 2U) * PROXY_SECTORS_PER_CLUSTER +
			relative % PROXY_SECTORS_PER_CLUSTER) * PROXY_SECTOR_SIZE;
		uint32_t node_index = proxy_cluster_node(cluster);
		uint64_t offset;
		const struct proxy_node *node;

		if (cluster >= proxy.bitmap_cluster &&
		    cluster < proxy.bitmap_cluster + proxy.bitmap_clusters) {
			uint64_t bitmap_offset = cluster_offset -
				(uint64_t)(proxy.bitmap_cluster - 2U) * PROXY_CLUSTER_SIZE;
			size_t length = bitmap_offset < proxy.bitmap_bytes ? proxy.bitmap_bytes - bitmap_offset : 0U;

			length = length < PROXY_SECTOR_SIZE ? length : PROXY_SECTOR_SIZE;
			memset(sector, 0, PROXY_SECTOR_SIZE);
			if (length != 0U) {
				memcpy(sector, proxy.bitmap + bitmap_offset, length);
			}
			return 0;
		}
		if (cluster >= proxy.upcase_cluster &&
		    cluster < proxy.upcase_cluster + proxy.upcase_clusters) {
			uint64_t upcase_offset = cluster_offset -
				(uint64_t)(proxy.upcase_cluster - 2U) * PROXY_CLUSTER_SIZE;
			size_t length = upcase_offset < proxy.upcase_bytes ?
				proxy.upcase_bytes - upcase_offset : 0U;

			if (length > PROXY_SECTOR_SIZE) {
				length = PROXY_SECTOR_SIZE;
			}
			memset(sector, 0, PROXY_SECTOR_SIZE);
			if (length != 0U) {
				memcpy(sector, proxy.upcase + upcase_offset, length);
			}
			return 0;
		}

		if (node_index == PROXY_INVALID_NODE) {
			memset(sector, 0, PROXY_SECTOR_SIZE);
			return 0;
		}
		node = &proxy.nodes[node_index];
		offset = ((uint64_t)(cluster - node->first_cluster) * PROXY_SECTORS_PER_CLUSTER +
			  relative % PROXY_SECTORS_PER_CLUSTER) * PROXY_SECTOR_SIZE;
		return node->directory ? proxy_read_directory(node_index, offset, sector) :
			proxy_read_file(node_index, offset, sector);
	}
}


/**
 * @brief Read a consecutive run of sectors of one source file in one call.
 *
 * @details Sectors of one source file that follow each other are read with a
 * single call instead of one call per sector. The run is clamped to the end of
 * the node's extent and to 128 sectors, zeroed first, and truncated to the
 * file size; the cached descriptor is positioned once and read in a loop.
 * Anything that is not a plain VFS file falls back to @c proxy_read_sector().
 *
 * @param[in]  sector Logical sector to start at.
 * @param[out] output Buffer receiving the sectors.
 * @param[in]  count  Maximum number of sectors to read.
 * @return Number of sectors filled (at least one), or a negative errno.
 */
static int proxy_read_run(uint64_t sector, uint8_t *output, uint32_t count)
{
	uint32_t cluster;
	uint32_t node_index;
	const struct proxy_node *node;
	uint64_t end_sector;
	uint64_t offset;
	uint32_t sectors;
	size_t length;
	size_t got = 0U;

	if (sector < proxy.data_start || sector >= proxy.total_sectors) {
		return proxy_read_sector(sector, output) == 0 ? 1 : -EIO;
	}
	cluster = (uint32_t)((sector - proxy.data_start) / PROXY_SECTORS_PER_CLUSTER) + 2U;
	node_index = proxy_cluster_node(cluster);
	if (node_index == PROXY_INVALID_NODE) {
		return proxy_read_sector(sector, output) == 0 ? 1 : -EIO;
	}
	node = &proxy.nodes[node_index];
	if (node->directory || node->source != PROXY_SOURCE_VFS || node->retired || node->tombstone) {
		return proxy_read_sector(sector, output) == 0 ? 1 : -EIO;
	}
	end_sector = proxy.data_start + (uint64_t)(node->first_cluster + node->cluster_count - 2U) * PROXY_SECTORS_PER_CLUSTER;
	sectors = end_sector - sector < count ? (uint32_t)(end_sector - sector) : count;
	if (sectors > 128U) {
		sectors = 128U;
	}
	offset = ((uint64_t)(cluster - node->first_cluster) * PROXY_SECTORS_PER_CLUSTER +
		(sector - proxy.data_start) % PROXY_SECTORS_PER_CLUSTER) * PROXY_SECTOR_SIZE;
	length = (size_t)sectors * PROXY_SECTOR_SIZE;
	memset(output, 0, length);
	if (offset >= node->size) {
		return (int)sectors;
	}
	if (node->size - offset < length) {
		length = (size_t)(node->size - offset);
	}
	if (proxy_open_cached(node_index, offset) != 0) {
		return -EIO;
	}
	while (got < length) {
		int result = vfs_read(proxy.cached_fd, output + got, length - got);

		if (result <= 0) {
			return result == 0 ? -EIO : result;
		}
		got += (size_t)result;
		proxy.cached_position += (size_t)result;
	}
	return (int)sectors;
}


/**
 * @brief Block device read callback of the view.
 *
 * @details The lock is taken only when the view is running, which is what lets
 * teardown call the lock-free path. Under the lock any queued write is flushed
 * first, so the host never reads data older than what it just wrote. The read
 * then proceeds in runs and stops at the first error.
 *
 * @param[in]  disk   Disk being read; unused.
 * @param[in]  sector First logical sector.
 * @param[out] buffer Buffer receiving the data.
 * @param[in]  count  Number of sectors to read.
 * @return 0 on success, or a negative errno.
 */
static int proxy_disk_read(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count)
{
	uint8_t *output = buffer;
	bool locked = proxy.running;
	int result = 0;

	(void)disk;
	if (locked) {
		mutex_lock_timeout(&proxy.lock, WAIT_FOREVER);
	}
	if (locked) {
		result = proxy_write_flush();
	}
	for (uint32_t index = 0U; result == 0 && index < count;) {
		int served = proxy_read_run(sector + index, output + (size_t)index * PROXY_SECTOR_SIZE, count - index);

		if (served <= 0) {
			result = served < 0 ? served : -EIO;
			break;
		}
		index += (uint32_t)served;
	}
	if (locked) {
		mutex_unlock(&proxy.lock);
	}
	return result;
}


/**
 * @brief Forget the cached read descriptor and position.
 *
 * @details A cached read descriptor and position are stale once the file under
 * it was written or resized.
 */
void proxy_drop_read_cache(void)
{
	if (proxy.cached_fd >= 0) {
		vfs_close(proxy.cached_fd);
		proxy.cached_fd = -1;
	}
	proxy.cached_node = PROXY_INVALID_NODE;
}


/**
 * @brief Fill a file with zeros up to a position before writing there.
 *
 * @details Some file systems cannot seek past the end of a file. Data may
 * arrive out of order, so a target shorter than the place being written to is
 * first filled with zeros up to it; they read as zeros either way. A stat
 * failure is ignored because the seek that follows reports what is wrong. The
 * zero buffer is the shared fill buffer, written in chunks until the position
 * is reached.
 *
 * @param[in] descriptor Open descriptor of the file.
 * @param[in] position   Position the following write will use.
 * @return 0 on success, or a negative errno.
 */
int proxy_fill_to(int descriptor, uint64_t position)
{
	struct vfs_stat status;
	uint64_t size;

	if (vfs_fstat(descriptor, &status) != 0) {
		return 0; /* the seek that follows reports what is wrong */
	}
	size = status.size;
	if (size >= position) {
		return 0;
	}
	if (vfs_lseek(descriptor, size, 0) < 0) {
		return -EIO;
	}
	memset(proxy.fill_buffer, 0, sizeof(proxy.fill_buffer));
	while (size < position) {
		size_t length = position - size < sizeof(proxy.fill_buffer) ? (size_t)(position - size) : sizeof(proxy.fill_buffer);
		int written = vfs_write(descriptor, proxy.fill_buffer, length);

		if (written <= 0) {
			return written < 0 ? written : -EIO;
		}
		size += (uint64_t)written;
	}
	return 0;
}


/**
 * @brief Write one sector of the view by its sector number.
 *
 * @details Writes to a read-only or out-of-range sector fail. Only the volume
 * flags of the boot region are kept, because the geometry is fixed. FAT writes
 * are ignored: every object is advertised as a contiguous NoFatChain stream.
 * Bitmap writes both update the stored bitmap and drop any staged data for
 * clusters that just became free. A data cluster owned by a node is written
 * back through the directory or file path; one that no node owns is stored as
 * orphan data until its entry arrives.
 *
 * @param[in] sector_number Logical sector number inside the view.
 * @param[in] data          Sector contents.
 * @return 0 on success, or a negative errno.
 */
static int proxy_write_sector(uint64_t sector_number, const uint8_t data[PROXY_SECTOR_SIZE])
{
	if (proxy.source_lost) {
		return -ENODEV;
	}
	if (sector_number >= proxy.total_sectors || proxy.read_only) {
		return proxy.read_only ? -EROFS : -EINVAL;
	}
	if (sector_number < PROXY_BOOT_SECTORS) {
		if (sector_number % 12U == 0U) {
			proxy.volume_flags = get_le16(data + 106U);
		}
		return 0;
	}
	if (sector_number < PROXY_FAT_OFFSET + proxy.fat_sectors) {
		return 0; /* Objects are advertised as contiguous NoFatChain streams. */
	}
	if (sector_number < proxy.data_start) {
		return 0;
	}
	{
		uint64_t relative = sector_number - proxy.data_start;
		uint32_t cluster = (uint32_t)(relative / PROXY_SECTORS_PER_CLUSTER) + 2U;
		uint16_t sector_in_cluster = (uint16_t)(relative % PROXY_SECTORS_PER_CLUSTER);
		uint64_t cluster_offset = (uint64_t)(cluster - 2U) * PROXY_CLUSTER_SIZE +
			(uint64_t)sector_in_cluster * PROXY_SECTOR_SIZE;
		uint32_t node_index;

		if (cluster >= proxy.bitmap_cluster && cluster < proxy.bitmap_cluster + proxy.bitmap_clusters) {
			uint64_t bitmap_offset = cluster_offset -
				(uint64_t)(proxy.bitmap_cluster - 2U) * PROXY_CLUSTER_SIZE;
			size_t length = bitmap_offset < proxy.bitmap_bytes ? proxy.bitmap_bytes - bitmap_offset : 0U;

			if (length != 0U) {
				proxy_bitmap_freed(bitmap_offset, data, length < PROXY_SECTOR_SIZE ? length : PROXY_SECTOR_SIZE);
				memcpy(proxy.bitmap + bitmap_offset, data,
					length < PROXY_SECTOR_SIZE ? length : PROXY_SECTOR_SIZE);
			}
			return 0;
		}
		if (cluster >= proxy.upcase_cluster && cluster < proxy.upcase_cluster + proxy.upcase_clusters) {
			return 0;
		}
		node_index = proxy_cluster_node(cluster);
		if (node_index == PROXY_INVALID_NODE) {
			return proxy_orphan_store(cluster, sector_in_cluster, data);
		}
		{
			struct proxy_node *node = &proxy.nodes[node_index];
			uint64_t offset = ((uint64_t)(cluster - node->first_cluster) * PROXY_SECTORS_PER_CLUSTER +
				sector_in_cluster) * PROXY_SECTOR_SIZE;

			return node->directory ? proxy_write_directory(node_index, offset, data) :
				proxy_write_node(node_index, offset, data);
		}
	}
}


/**
 * @brief Block device write callback of the view.
 *
 * @details The whole request is handled under @c proxy.lock, one sector at a
 * time, so a directory sector and the data it references cannot interleave
 * with another writer. The sector counter is updated and the first eight
 * failures are logged.
 *
 * @param[in] disk   Disk being written; unused.
 * @param[in] sector First logical sector.
 * @param[in] buffer Data to write.
 * @param[in] count  Number of sectors.
 * @return 0 on success, or a negative errno of the first failing sector.
 */
static int proxy_disk_write(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count)
{
	const uint8_t *input = buffer;
	int result = 0;

	uint64_t begin = proxy_time_begin();

	(void)disk;
	mutex_lock_timeout(&proxy.lock, WAIT_FOREVER);
	for (uint32_t index = 0U; index < count; index++) {
		result = proxy_write_sector(sector + index, input + (size_t)index * PROXY_SECTOR_SIZE);
		if (result != 0) {
			break;
		}
	}
	proxy_stats.sectors += count;
	if (result != 0 && proxy_stats.write_errors++ < 8U) {
		pr_warn("proxy: write at sector %llu count %u failed: %d\n", (unsigned long long)sector, (uint32_t)count, result);
	}
	mutex_unlock(&proxy.lock);
	proxy_time_end(&proxy_stats.disk_write, begin);
	return result;
}


/**
 * @brief Block device flush callback of the view.
 *
 * @details Queued file data is written first, then the deletes the host asked
 * for are carried out, so a flush leaves the source volumes consistent. The
 * whole sequence runs under @c proxy.lock and failures are logged.
 *
 * @param[in] disk Disk being flushed; unused.
 * @return 0 on success, or a negative errno.
 */
static int proxy_disk_flush(struct gendisk *disk)
{
	int result;

	(void)disk;
	mutex_lock_timeout(&proxy.lock, WAIT_FOREVER);
	result = proxy_write_flush();
	if (result == 0) {
		result = proxy_apply_deletes();
	}
	if (result != 0) {
		pr_warn("proxy: flush failed: %d\n", result);
	}
	mutex_unlock(&proxy.lock);
	return result;
}


/**
 * @brief Block device free callback of the view.
 *
 * @details Nothing is tied to the disk object itself; the view is released by
 * @c storage_proxy_stop().
 *
 * @param[in] disk Disk being freed; unused.
 */
static void proxy_disk_free(struct gendisk *disk)
{
	(void)disk;
}

/** @brief Block device operations the view registers under @c PROXY_DISK_NAME. */
const struct block_device_operations proxy_disk_operations = {
	.read = proxy_disk_read,
	.write = proxy_disk_write,
	.flush = proxy_disk_flush,
	.free_disk = proxy_disk_free,
};
