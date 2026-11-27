// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_proxy_test.c
 * @brief Host test for the lazy exFAT proxy, read path and write path.
 *
 * Builds a RAM card with a FAT32 and an ext4 partition, starts the lazy view,
 * mounts @c cardview1 through the VFS and walks it. The nested source directory
 * holds more than one thousand files, so the test proves that a view directory
 * can span several 32 KiB clusters. File bytes are compared with the source
 * patterns, the allocation bitmap is checked to have every reservation used and
 * the tail free, and the stop/start cycle is repeated.
 *
 * The write half creates, edits, renames and deletes objects through the view,
 * creates several hundred files in one directory (forcing a host directory
 * extension), drops every cache to prove the overlay survives eviction, and
 * then verifies the source tree and a fresh view.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/blkdev.h>
#include <vfs.h>

#include "storage/proxy_lazy.h"

/** @brief Size of the simulated card in 512-byte sectors (160 MiB). */
#define CARD_SECTORS (160U * 2048U)
/** @brief Size of each simulated source partition in MiB. */
#define PARTITION_MIB 64U
/** @brief Cluster size of the lazy view, in bytes. */
#define VIEW_CLUSTER_BYTES 4096U
/** @brief Sectors per cluster of the lazy view. */
#define VIEW_SECTORS_PER_CLUSTER (VIEW_CLUSTER_BYTES / 512U)
/** @brief Files created in the nested directory that must span several view clusters. */
#define MANY_FILES 1001U
/** @brief Files created in the second large directory used to force LRU eviction. */
#define MANY2_FILES 200U
/** @brief Files created in the directory that a small entry cap truncates. */
#define HUGE_FILES 50U
/** @brief Files created in the second partition's nested directory. */
#define EXT_FILES 12U

/** @brief Backing store of the simulated card, allocated in main(). */
static uint8_t *disk_data;
/** @brief Number of failed assertions so far. */
static uint32_t failures;
/** @brief Number of assertions executed so far. */
static uint32_t checks;

/**
 * @brief Assert a condition, counting it and reporting the failing line.
 *
 * @param[in] condition Expression expected to be true.
 */
#define CHECK(condition)                                                               \
	do {                                                                             \
		checks++;                                                                 \
		if (!(condition)) {                                                       \
			printf("FAIL line %d: %s\n", __LINE__, #condition);               \
			failures++;                                                        \
		}                                                                        \
	} while (0)

/** @brief Host stub for the block-layer lock. */
void base_lock(void) {}
/** @brief Host stub for the block-layer unlock. */
void base_unlock(void) {}
/** @brief Host stub for the VFS lock. */
void vfs_os_lock(void) {}
/** @brief Host stub for the VFS unlock. */
void vfs_os_unlock(void) {}

/**
 * @brief Host implementation of the kernel logger that discards messages.
 *
 * @param[in] level  Log level; ignored.
 * @param[in] module Module name; ignored.
 * @param[in] format Message format; ignored.
 * @param[in] ...    Arguments for @p format; ignored.
 */
void log_print(int level, const char *module, const char *format, ...)
{
	(void)level;
	(void)module;
	(void)format;
}

/**
 * @brief Read sectors from the in-memory card.
 *
 * @param[in]  disk   Disk being read; ignored.
 * @param[in]  sector First sector to read.
 * @param[out] buffer Destination for @p count sectors.
 * @param[in]  count  Number of 512-byte sectors.
 * @return 0.
 */
static int ram_read(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count)
{
	(void)disk;
	memcpy(buffer, disk_data + (size_t)sector * 512U, (size_t)count * 512U);
	return 0;
}

/**
 * @brief Write sectors to the in-memory card.
 *
 * @param[in] disk   Disk being written; ignored.
 * @param[in] sector First sector to write.
 * @param[in] buffer Source of @p count sectors.
 * @param[in] count  Number of 512-byte sectors.
 * @return 0.
 */
static int ram_write(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count)
{
	(void)disk;
	memcpy(disk_data + (size_t)sector * 512U, buffer, (size_t)count * 512U);
	return 0;
}

/** @brief Block operations of the simulated card. */
static const struct block_device_operations ram_operations = {
	.read = ram_read,
	.write = ram_write,
};

/** @brief Registers the FatFs-backed VFS implementation. */
extern int vfs_ff_register(void);
/** @brief Registers the ext4-backed VFS implementation. */
extern int vfs_ext_register(void);

/**
 * @brief Deterministic content byte for a file at a given offset.
 *
 * @param[in] seed     Per-file seed.
 * @param[in] position Byte offset in the file.
 * @return The pattern byte at @p position.
 */
static uint8_t pattern_byte(uint32_t seed, size_t position)
{
	return (uint8_t)((position * 29U + seed * 131U + (position >> 9) * 7U) + 7U);
}

/**
 * @brief Write a deterministic pattern to a file in chunks.
 *
 * @param[in] path  Destination path.
 * @param[in] size  File size in bytes.
 * @param[in] seed  Pattern seed.
 * @param[in] chunk Maximum write size in bytes.
 * @return 0 on success, or a negative errno.
 */
static int write_pattern(const char *path, size_t size, uint32_t seed, size_t chunk)
{
	uint8_t *buffer = malloc(chunk);
	int descriptor = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
	size_t offset = 0U;
	int result = 0;

	if (descriptor < 0 || buffer == NULL) {
		free(buffer);
		return descriptor < 0 ? descriptor : -ENOMEM;
	}
	while (result == 0 && offset < size) {
		size_t length = size - offset < chunk ? size - offset : chunk;

		for (size_t index = 0U; index < length; index++) {
			buffer[index] = pattern_byte(seed, offset + index);
		}
		result = vfs_write(descriptor, buffer, length) == (int)length ? 0 : -EIO;
		offset += length;
	}
	free(buffer);
	{
		int close_result = vfs_close(descriptor);

		return result != 0 ? result : close_result;
	}
}

/**
 * @brief Verify a file written by write_pattern().
 *
 * @param[in] path File to verify.
 * @param[in] size Expected size in bytes.
 * @param[in] seed Expected pattern seed.
 * @return 0 when the file matches, the first differing offset + 1 when a byte
 *         differs, or a negative errno on open, read, or size mismatch.
 */
static long verify_pattern(const char *path, size_t size, uint32_t seed)
{
	uint8_t block[4096];
	int descriptor = vfs_open(path, VFS_O_READ);
	size_t offset = 0U;
	long result = 0;

	if (descriptor < 0) {
		return descriptor;
	}
	while (result == 0 && offset < size) {
		size_t length = size - offset < sizeof(block) ? size - offset : sizeof(block);

		if (vfs_read(descriptor, block, length) != (int)length) {
			result = -EIO;
			break;
		}
		for (size_t index = 0U; index < length; index++) {
			if (block[index] != pattern_byte(seed, offset + index)) {
				result = (long)(offset + index) + 1;
				break;
			}
		}
		offset += length;
	}
	{
		struct vfs_stat stat;

		if (result == 0 && vfs_fstat(descriptor, &stat) == 0 && stat.size != size) {
			result = -EFBIG;
		}
	}
	vfs_close(descriptor);
	return result;
}

/**
 * @brief Write a deterministic pattern at an offset of an existing file.
 *
 * @param[in] path   Destination path.
 * @param[in] offset Byte offset of the first byte.
 * @param[in] size   Number of bytes to write.
 * @param[in] seed   Pattern seed.
 * @param[in] chunk  Maximum write size in bytes.
 * @return 0 on success, or a negative errno.
 */
static int write_at(const char *path, size_t offset, size_t size, uint32_t seed, size_t chunk)
{
	uint8_t *buffer = malloc(chunk);
	int descriptor = vfs_open(path, VFS_O_WRITE);
	size_t written = 0U;
	int result = 0;

	if (descriptor < 0 || buffer == NULL) {
		free(buffer);
		return descriptor < 0 ? descriptor : -ENOMEM;
	}
	if (vfs_lseek(descriptor, (int64_t)offset, 0) < 0) {
		result = -EIO;
	}
	while (result == 0 && written < size) {
		size_t length = size - written < chunk ? size - written : chunk;

		for (size_t index = 0U; index < length; index++) {
			buffer[index] = pattern_byte(seed, offset + written + index);
		}
		result = vfs_write(descriptor, buffer, length) == (int)length ? 0 : -EIO;
		written += length;
	}
	free(buffer);
	{
		int close_result = vfs_close(descriptor);

		return result != 0 ? result : close_result;
	}
}

/**
 * @brief Append a deterministic pattern to a file.
 *
 * @param[in] path  Destination path.
 * @param[in] size  Number of bytes to append.
 * @param[in] seed  Pattern seed.
 * @param[in] chunk Maximum write size in bytes.
 * @return 0 on success, or a negative errno.
 */
static int append_pattern(const char *path, size_t size, uint32_t seed, size_t chunk)
{
	uint8_t *buffer = malloc(chunk);
	int descriptor = vfs_open(path, VFS_O_WRITE);
	int64_t offset;
	size_t written = 0U;
	int result = 0;

	if (descriptor < 0 || buffer == NULL) {
		free(buffer);
		return descriptor < 0 ? descriptor : -ENOMEM;
	}
	offset = vfs_lseek(descriptor, 0, 2);
	if (offset < 0) {
		result = (int)offset;
	}
	while (result == 0 && written < size) {
		size_t length = size - written < chunk ? size - written : chunk;

		for (size_t index = 0U; index < length; index++) {
			buffer[index] = pattern_byte(seed,
				(size_t)offset + written + index);
		}
		result = vfs_write(descriptor, buffer, length) == (int)length ? 0 : -EIO;
		written += length;
	}
	free(buffer);
	{
		int close_result = vfs_close(descriptor);

		return result != 0 ? result : close_result;
	}
}

/**
 * @brief One expected byte range of a file.
 */
struct expected_range {
	/** @brief First byte of the range. */
	size_t start;
	/** @brief Number of bytes in the range. */
	size_t length;
	/** @brief Pattern seed used for the range. */
	uint32_t seed;
};

/**
 * @brief Verify a file made of several pattern ranges.
 *
 * @param[in] path   File to verify.
 * @param[in] size   Expected size in bytes.
 * @param[in] ranges Expected ranges, in ascending order.
 * @param[in] count  Number of ranges.
 * @return 0 when the file matches, a positive byte offset when one differs,
 *         or a negative errno.
 */
static long verify_ranges(const char *path, size_t size, const struct expected_range *ranges,
	size_t count)
{
	uint8_t block[4096];
	int descriptor = vfs_open(path, VFS_O_READ);
	size_t offset = 0U;
	long result = 0;

	if (descriptor < 0) {
		return descriptor;
	}
	while (result == 0 && offset < size) {
		size_t length = size - offset < sizeof(block) ? size - offset : sizeof(block);

		if (vfs_read(descriptor, block, length) != (int)length) {
			result = -EIO;
			break;
		}
		for (size_t index = 0U; index < length; index++) {
			size_t position = offset + index;
			uint8_t expected = 0U;
			bool known = false;

			for (size_t range = 0U; range < count; range++) {
				if (position >= ranges[range].start &&
				    position < ranges[range].start + ranges[range].length) {
					expected = pattern_byte(ranges[range].seed, position);
					known = true;
					break;
				}
			}
			if (!known) {
				expected = (uint8_t)(position * 29U + 7U);
			}
			if (block[index] != expected) {
				result = (long)position + 1;
				break;
			}
		}
		offset += length;
	}
	{
		struct vfs_stat stat;

		if (result == 0 && vfs_fstat(descriptor, &stat) == 0 && stat.size != size) {
			result = -EFBIG;
		}
	}
	vfs_close(descriptor);
	return result;
}

/**
 * @brief Truncate a file through the VFS.
 *
 * @param[in] path File path.
 * @param[in] size New size.
 * @return 0 on success, or a negative errno.
 */
static int truncate_file(const char *path, size_t size)
{
	int descriptor = vfs_open(path, VFS_O_WRITE);
	int result;

	if (descriptor < 0) {
		return descriptor;
	}
	result = vfs_ftruncate(descriptor, size);
	{
		int close_result = vfs_close(descriptor);

		return result != 0 ? result : close_result;
	}
}

/**
 * @brief Count the entries of a view or source directory.
 *
 * @param[in] path Directory path.
 * @return The entry count, or UINT32_MAX when the directory cannot be opened.
 */
static uint32_t count_entries(const char *path)
{
	struct vfs_dir *dir;
	struct vfs_dirent entry;
	uint32_t count = 0U;
	int error = 0;

	dir = vfs_opendir(path, &error);
	if (dir == NULL) {
		printf("cannot open %s: %d\n", path, error);
		return UINT32_MAX;
	}
	while (vfs_readdir(dir, &entry) == 0) {
		count++;
	}
	vfs_closedir(dir);
	return count;
}

/**
 * @brief Report whether a directory contains an exact name.
 *
 * @param[in] path Directory path.
 * @param[in] name Name to look for.
 * @return True when the name appears.
 */
static bool directory_has(const char *path, const char *name)
{
	struct vfs_dir *dir;
	struct vfs_dirent entry;
	bool found = false;
	int error = 0;

	dir = vfs_opendir(path, &error);
	if (dir == NULL) {
		printf("cannot open %s: %d\n", path, error);
		return false;
	}
	while (vfs_readdir(dir, &entry) == 0) {
		if (strcmp(entry.name, name) == 0) {
			found = true;
			break;
		}
	}
	vfs_closedir(dir);
	return found;
}

/**
 * @brief Verify every file of the nested multi-cluster directory.
 *
 * @details Each name must parse as @c fNNNN.bin and appear exactly once. The
 * whole set must be visible, which is only possible when the view directory
 * spans more than one 32 KiB cluster.
 *
 * @param[in] path View path of the directory.
 * @return True when the directory holds exactly MANY_FILES distinct files.
 */
static bool check_many_directory(const char *path)
{
	uint8_t *seen = calloc(MANY_FILES, 1U);
	struct vfs_dir *dir;
	struct vfs_dirent entry;
	bool okay = seen != NULL;
	uint32_t count = 0U;
	int error = 0;

	dir = vfs_opendir(path, &error);
	if (dir == NULL || seen == NULL) {
		printf("cannot open %s: %d\n", path, error);
		free(seen);
		return false;
	}
	while (vfs_readdir(dir, &entry) == 0) {
		unsigned int number = 0U;
		char suffix = '\0';

		count++;
		if (sscanf(entry.name, "f%u.bin%c", &number, &suffix) != 1U || suffix != '\0' ||
		    number >= MANY_FILES || seen[number] != 0U) {
			okay = false;
			break;
		}
		seen[number] = 1U;
	}
	vfs_closedir(dir);
	if (count != MANY_FILES) {
		printf("%s holds %u entries, expected %u\n", path, count, MANY_FILES);
		okay = false;
	}
	for (uint32_t index = 0U; okay && index < MANY_FILES; index++) {
		if (seen[index] == 0U) {
			printf("%s is missing f%04u.bin\n", path, index);
			okay = false;
		}
	}
	free(seen);
	return okay;
}

/**
 * @brief Create the simulated card, its two partitions and their source trees.
 *
 * @param[out] disk_out Receives the allocated disk.
 */
static void build_card(struct gendisk **disk_out)
{
	/** @brief Filesystem of each source partition. */
	static const char *const types[2] = { "fat", "ext" };
	struct blk_partition_layout_entry layout[2] = {
		{ .sectors = PARTITION_MIB * 2048U, .content = BLK_PARTITION_FAT32, .label = "FATSRC" },
		{ .sectors = PARTITION_MIB * 2048U, .content = BLK_PARTITION_EXT4, .label = "EXTSRC" },
	};
	struct gendisk *disk = alloc_disk();

	memset(disk_data, 0, (size_t)CARD_SECTORS * 512U);
	CHECK(disk != NULL);
	strcpy(disk->disk_name, "mmcblk0");
	disk->fops = &ram_operations;
	disk->capacity = CARD_SECTORS;
	CHECK(add_disk(disk) == 0);
	CHECK(blk_write_partition_layout(disk, BLK_PARTITION_TABLE_MBR, layout, 2U) == 0);
	for (uint32_t partition = 1U; partition <= 2U; partition++) {
		char device[24];

		snprintf(device, sizeof(device), "mmcblk0p%u", partition);
		CHECK(vfs_format(device, types[partition - 1U], NULL, 0U) == 0);
	}
	CHECK(vfs_mount("mmcblk0p1", "/src1", "fat") == 0);
	CHECK(write_pattern("/src1/seed1.bin", 70000U, 1U, 4096U) == 0);
	CHECK(vfs_mkdir("/src1/many") == 0);
	for (uint32_t index = 0U; index < MANY_FILES; index++) {
		char path[64];
		size_t size = 100U + index % 13U;

		snprintf(path, sizeof(path), "/src1/many/f%04u.bin", index);
		CHECK(write_pattern(path, size, 1000U + index, 256U) == 0);
	}
	CHECK(vfs_mkdir("/src1/many2") == 0);
	for (uint32_t index = 0U; index < MANY2_FILES; index++) {
		char path[64];
		size_t size = 400U + index % 5U;

		snprintf(path, sizeof(path), "/src1/many2/g%03u.bin", index);
		CHECK(write_pattern(path, size, 6000U + index, 256U) == 0);
	}
	CHECK(vfs_mkdir("/src1/huge") == 0);
	for (uint32_t index = 0U; index < HUGE_FILES; index++) {
		char path[64];
		size_t size = 120U + index % 9U;

		snprintf(path, sizeof(path), "/src1/huge/h%04u.bin", index);
		CHECK(write_pattern(path, size, 7000U + index, 128U) == 0);
	}
	CHECK(vfs_mkdir("/src1/tree") == 0);
	CHECK(vfs_mkdir("/src1/tree/sub") == 0);
	CHECK(write_pattern("/src1/tree/sub/deep.bin", 40000U, 77U, 4096U) == 0);
	CHECK(vfs_umount("/src1") == 0);

	CHECK(vfs_mount("mmcblk0p2", "/src2", "ext") == 0);
	CHECK(write_pattern("/src2/seed2.bin", 65000U, 2U, 4096U) == 0);
	CHECK(vfs_mkdir("/src2/nested") == 0);
	for (uint32_t index = 0U; index < EXT_FILES; index++) {
		char path[64];
		size_t size = 300U + index * 37U;

		snprintf(path, sizeof(path), "/src2/nested/file-%02u.bin", index);
		CHECK(write_pattern(path, size, 3000U + index, 256U) == 0);
	}
	CHECK(vfs_umount("/src2") == 0);
	*disk_out = disk;
}

/**
 * @brief Read a little-endian 32-bit value.
 *
 * @param[in] bytes Four bytes in little-endian order.
 * @return The decoded value.
 */
static uint32_t test_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * @brief Read a little-endian 64-bit value.
 *
 * @param[in] bytes Eight bytes in little-endian order.
 * @return The decoded value.
 */
static uint64_t test_le64(const uint8_t *bytes)
{
	uint64_t value = 0U;

	for (uint32_t index = 0U; index < 8U; index++) {
		value |= (uint64_t)bytes[index] << (index * 8U);
	}
	return value;
}

/**
 * @brief Find a directory entry in a view directory extent and return its extent.
 *
 * @details The whole extent is read in one call and every slot is scanned, so
 * an entry group that crosses a sector boundary is still found.
 *
 * @param[in]  view          Open @c cardview1 block device.
 * @param[in]  layout        View geometry.
 * @param[in]  first         First cluster of the directory extent.
 * @param[in]  clusters      Number of clusters of the directory extent.
 * @param[in]  name          ASCII name to look for.
 * @param[out] first_cluster Receives the entry's first cluster, or NULL.
 * @param[out] entry_clusters Receives the entry's cluster count, or NULL.
 * @param[out] length        Receives the DataLength, or NULL.
 * @return True when the entry was found.
 */
static bool view_entry_extent(struct block_device *view,
	const struct storage_proxy_lazy_layout *layout, uint32_t first, uint32_t clusters,
	const char *name, uint32_t *first_cluster, uint32_t *entry_clusters, uint64_t *length)
{
	size_t sectors = (size_t)clusters * VIEW_SECTORS_PER_CLUSTER;
	size_t slots = sectors * 16U;
	size_t name_length = strlen(name);
	uint8_t *buffer;
	bool found = false;

	if (sectors == 0U) {
		return false;
	}
	buffer = aligned_alloc(64U, sectors * 512U);
	if (buffer == NULL) {
		return false;
	}
	if (bdev_read(view, layout->data_start +
		(uint64_t)(first - 2U) * VIEW_SECTORS_PER_CLUSTER, buffer,
		(uint32_t)sectors) != 0) {
		free(buffer);
		return false;
	}
	for (size_t slot = 0U; slot + 2U < slots; slot++) {
		const uint8_t *entry = buffer + slot * 32U;
		const uint8_t *stream = entry + 32U;
		const uint8_t *name_entry = entry + 64U;
		bool same = true;

		if (entry[0] != 0x85U || stream[0] != 0xc0U || name_entry[0] != 0xc1U ||
		    stream[3] != name_length) {
			continue;
		}
		for (size_t index = 0U; same && index < name_length; index++) {
			same = name_entry[2U + index * 2U] == (uint8_t)name[index] &&
				name_entry[3U + index * 2U] == 0U;
		}
		if (same) {
			uint64_t data_length = test_le64(stream + 24U);

			if (first_cluster != NULL) {
				*first_cluster = test_le32(stream + 20U);
			}
			if (entry_clusters != NULL) {
				*entry_clusters = (uint32_t)((data_length + VIEW_CLUSTER_BYTES - 1U) /
					VIEW_CLUSTER_BYTES);
			}
			if (length != NULL) {
				*length = data_length;
			}
			found = true;
			break;
		}
	}
	free(buffer);
	return found;
}

/**
 * @brief Find a directory entry in a view directory extent and return its data length.
 *
 * @details The whole extent is read in one call and every slot is scanned, so
 * an entry group that crosses a sector boundary is still found.
 *
 * @param[in] view     Open @c cardview1 block device.
 * @param[in] layout   View geometry.
 * @param[in] first    First cluster of the directory extent.
 * @param[in] clusters Number of clusters of the directory extent.
 * @param[in] name     ASCII name to look for.
 * @return The DataLength of the entry, or zero when it was not found.
 */
static uint64_t view_entry_length(struct block_device *view,
	const struct storage_proxy_lazy_layout *layout, uint32_t first, uint32_t clusters,
	const char *name)
{
	uint64_t length = 0U;

	(void)view_entry_extent(view, layout, first, clusters, name, NULL, NULL, &length);
	return length;
}

/**
 * @brief Report whether one cluster is marked used in a view bitmap.
 *
 * @param[in] bitmap  Bitmap bytes.
 * @param[in] cluster Cluster number, two or greater.
 * @return True when the bit is set.
 */
static bool bitmap_used(const uint8_t *bitmap, uint32_t cluster)
{
	uint32_t bit = cluster - 2U;

	return (bitmap[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}

/**
 * @brief Check that every reservation is used and only the tail is free.
 *
 * @param[in] view   Open @c cardview1 block device.
 * @param[in] layout View geometry.
 */
static void check_bitmap(struct block_device *view, const struct storage_proxy_lazy_layout *layout)
{
	size_t sectors = ((size_t)layout->bitmap_bytes + 511U) / 512U;
	uint8_t *bitmap;
	bool okay = true;

	CHECK(layout->tail_clusters > 0U);
	CHECK(layout->tail_first + layout->tail_clusters == layout->total_clusters + 2U);
	if (sectors == 0U) {
		return;
	}
	bitmap = aligned_alloc(64U, sectors * 512U);
	CHECK(bitmap != NULL);
	if (bitmap == NULL) {
		return;
	}
	memset(bitmap, 0, sectors * 512U);
	CHECK(bdev_read(view, layout->data_start +
		(uint64_t)(layout->bitmap_cluster - 2U) * VIEW_SECTORS_PER_CLUSTER,
		bitmap, (uint32_t)sectors) == 0);
	/* System areas and every reservation are used before the volume exists. */
	for (uint32_t cluster = layout->bitmap_cluster;
	     cluster < layout->root_cluster + layout->root_clusters; cluster++) {
		okay = okay && bitmap_used(bitmap, cluster);
	}
	for (uint32_t partition = 0U; partition < STORAGE_PROXY_MAX_PARTITIONS; partition++) {
		if (!layout->selected[partition]) {
			continue;
		}
		CHECK(layout->reserve_clusters[partition] > 0U);
		for (uint32_t cluster = layout->reserve_first[partition];
		     cluster < layout->reserve_first[partition] + layout->reserve_clusters[partition];
		     cluster++) {
			okay = okay && bitmap_used(bitmap, cluster);
		}
	}
	CHECK(okay);
	/* The tail is the only free space the host sees. */
	for (uint32_t cluster = layout->tail_first;
	     cluster < layout->tail_first + layout->tail_clusters; cluster++) {
		if (bitmap_used(bitmap, cluster)) {
			printf("tail cluster %u is marked used\n", cluster);
			CHECK(false);
			break;
		}
	}
	free(bitmap);
}

/**
 * @brief Read a whole small text file through the VFS.
 *
 * @param[in]  path   File to read.
 * @param[out] buffer Destination, always NUL-terminated on success.
 * @param[in]  size   Size of @p buffer in bytes.
 * @return Bytes read, or a negative errno.
 */
static int read_text_file(const char *path, char *buffer, size_t size)
{
	int descriptor = vfs_open(path, VFS_O_READ);
	int result;

	if (descriptor < 0) {
		return descriptor;
	}
	result = vfs_read(descriptor, buffer, size - 1U);
	vfs_close(descriptor);
	if (result < 0) {
		return result;
	}
	buffer[result] = '\0';
	return result;
}

/**
 * @brief Open and verify the synthesized root reports.
 *
 * @param[in] expect_truncated Whether the limit report must announce truncation.
 */
static void check_root_reports(bool expect_truncated)
{
	char text[2048];

	memset(text, 0, sizeof(text));
	CHECK(read_text_file("/lazy/_LAYOUT.TXT", text, sizeof(text)) > 0);
	CHECK(strstr(text, "total_clusters=") != NULL);
	CHECK(strstr(text, "reserved_clusters=") != NULL);
	CHECK(strstr(text, "tail_clusters=") != NULL);
	CHECK(strstr(text, "cache_entries_max=") != NULL);
	CHECK(strstr(text, "dir_max_entries=") != NULL);

	memset(text, 0, sizeof(text));
	CHECK(read_text_file("/lazy/_DIR_LIMIT.TXT", text, sizeof(text)) > 0);
	CHECK(strstr(text, "dir_max_entries=") != NULL);
	CHECK(strstr(text, "proxy.lazy.truncated=") != NULL);
	if (expect_truncated) {
		CHECK(strstr(text, "proxy.lazy.truncated=1") != NULL);
	} else {
		CHECK(strstr(text, "proxy.lazy.truncated=0") != NULL);
	}
}

/**
 * @brief Enumerate the view and verify the files and the multi-cluster directory.
 *
 * @param[in] view Open @c cardview1 block device.
 */
static void check_view_contents(struct block_device *view)
{
	struct storage_proxy_lazy_layout layout;

	CHECK(storage_proxy_lazy_layout(&layout) == 0);
	/* Two partition folders plus the two synthesized reports. */
	CHECK(count_entries("/lazy") == 4U);
	CHECK(directory_has("/lazy", "partition1"));
	CHECK(directory_has("/lazy", "partition2"));
	CHECK(directory_has("/lazy", "_LAYOUT.TXT"));
	CHECK(directory_has("/lazy", "_DIR_LIMIT.TXT"));
	check_root_reports(false);
	/* The synthesized reports are read-only at the block layer. */
	{
		uint32_t first = 0U;
		uint32_t clusters = 0U;
		uint8_t sector[512];

		CHECK(view_entry_extent(view, &layout, layout.root_cluster,
			layout.root_clusters, "_LAYOUT.TXT", &first, &clusters, NULL));
		CHECK(first != 0U);
		CHECK(clusters > 0U);
		memset(sector, 0x5a, sizeof(sector));
		CHECK(bdev_write(view, layout.data_start +
			(uint64_t)(first - 2U) * VIEW_SECTORS_PER_CLUSTER,
			sector, 1U) == -EROFS);
	}
	/* The root read reserved both partition folders. */
	CHECK(storage_proxy_lazy_layout(&layout) == 0);
	CHECK(layout.partition_root_clusters[0] > 0U);
	CHECK(layout.partition_root_clusters[1] > 0U);

	CHECK(directory_has("/lazy/partition1", "seed1.bin"));
	CHECK(directory_has("/lazy/partition1", "many"));
	CHECK(directory_has("/lazy/partition1", "many2"));
	CHECK(directory_has("/lazy/partition1", "huge"));
	CHECK(directory_has("/lazy/partition1", "tree"));
	CHECK(directory_has("/lazy/partition1/tree", "sub"));
	CHECK(directory_has("/lazy/partition1/tree/sub", "deep.bin"));
	CHECK(directory_has("/lazy/partition2", "seed2.bin"));
	CHECK(directory_has("/lazy/partition2", "nested"));

	/* More than one thousand files force a multi-cluster directory extent. */
	CHECK(check_many_directory("/lazy/partition1/many"));
	{
		uint64_t length = view_entry_length(view, &layout, layout.partition_root_first[0],
			layout.partition_root_clusters[0], "many");

		CHECK(length > (uint64_t)VIEW_CLUSTER_BYTES);
	}

	CHECK(verify_pattern("/lazy/partition1/seed1.bin", 70000U, 1U) == 0);
	CHECK(verify_pattern("/lazy/partition1/tree/sub/deep.bin", 40000U, 77U) == 0);
	CHECK(verify_pattern("/lazy/partition1/many/f0000.bin", 100U, 1000U) == 0);
	CHECK(verify_pattern("/lazy/partition1/many/f0500.bin", 100U + 500U % 13U, 1500U) == 0);
	CHECK(verify_pattern("/lazy/partition1/many/f1000.bin", 100U + 1000U % 13U, 2000U) == 0);
	CHECK(verify_pattern("/lazy/partition2/seed2.bin", 65000U, 2U) == 0);
	CHECK(verify_pattern("/lazy/partition2/nested/file-11.bin", 300U + 11U * 37U, 3011U) == 0);

	check_bitmap(view, &layout);
	/* The host sees exactly the tail as free space. */
	{
		uint64_t total_bytes = 0U;
		uint64_t free_bytes = 0U;

		CHECK(vfs_statfs("/lazy", &total_bytes, &free_bytes) == 0);
		CHECK(total_bytes == (uint64_t)layout.total_clusters * VIEW_CLUSTER_BYTES);
		CHECK(free_bytes == (uint64_t)layout.tail_clusters * VIEW_CLUSTER_BYTES);
	}
}

/** @brief Files created through the view in the bulk directory. */
#define BULK_FILES 400U
/** @brief Clusters in the data-first stream that must spill to a staging file. */
#define ORPHAN_CLUSTERS 256U
/** @brief Pattern seed of the data-first stream. */
#define ORPHAN_SEED 6001U

/**
 * @brief Compute the exFAT name hash of an ASCII file name.
 *
 * @param[in] name File name.
 * @return The 16-bit name hash.
 */
static uint16_t exfat_name_hash(const char *name)
{
	uint16_t hash = 0U;

	for (const uint8_t *character = (const uint8_t *)name; *character != 0U; character++) {
		uint16_t value = *character;

		if (value >= (uint16_t)'a' && value <= (uint16_t)'z') {
			value = (uint16_t)(value - ((uint16_t)'a' - (uint16_t)'A'));
		}
		hash = (uint16_t)(((hash & 1U) != 0U ? 0x8000U : 0U) + (hash >> 1) + value);
	}
	return hash;
}

/**
 * @brief Compute the exFAT entry-set checksum.
 *
 * @param[in] group   First entry of the set.
 * @param[in] entries Number of entries in the set.
 * @return The checksum stored in bytes 2 and 3 of the file entry.
 */
static uint16_t exfat_set_checksum(const uint8_t *group, uint32_t entries)
{
	uint16_t sum = 0U;

	for (uint32_t entry = 0U; entry < entries; entry++) {
		for (uint32_t byte = 0U; byte < 32U; byte++) {
			if (entry == 0U && (byte == 2U || byte == 3U)) {
				continue;
			}
			sum = (uint16_t)(((sum & 1U) != 0U ? 0x8000U : 0U) + (sum >> 1) +
				group[entry * 32U + byte]);
		}
	}
	return sum;
}

/**
 * @brief Store a little-endian value.
 *
 * @param[out] bytes Destination.
 * @param[in]  value Value to store.
 * @param[in]  width Number of bytes.
 */
static void store_le(uint8_t *bytes, uint64_t value, uint32_t width)
{
	for (uint32_t index = 0U; index < width; index++) {
		bytes[index] = (uint8_t)(value >> (index * 8U));
	}
}

/**
 * @brief Mark one view cluster used or free in the allocation bitmap.
 *
 * @param[in,out] view    Open @c cardview1 block device.
 * @param[in]     layout  View layout.
 * @param[in]     cluster Cluster to change.
 * @param[in]     used    New state.
 */
static void set_view_bitmap(struct block_device *view,
	const struct storage_proxy_lazy_layout *layout, uint32_t cluster, bool used)
{
	uint64_t bit = (uint64_t)cluster - 2U;
	uint64_t byte = bit / 8U;
	uint64_t sector = layout->data_start +
		(uint64_t)(layout->bitmap_cluster - 2U) * VIEW_SECTORS_PER_CLUSTER + byte / 512U;
	uint32_t offset = (uint32_t)(byte % 512U);
	uint8_t data[512];

	CHECK(bdev_read(view, sector, data, 1U) == 0);
	if (used) {
		data[offset] |= (uint8_t)(1U << (bit % 8U));
	} else {
		data[offset] &= (uint8_t)~(1U << (bit % 8U));
	}
	CHECK(bdev_write(view, sector, data, 1U) == 0);
}

/**
 * @brief Write a data-first stream into free tail clusters.
 *
 * @details The sectors are written without a directory entry, exactly what a
 * host does before it announces a new file. The pattern is the same one
 * write_pattern() uses, so the source file can be checked afterwards.
 *
 * @param[in,out] view    Open @c cardview1 block device.
 * @param[in]     layout  View layout.
 * @param[in]     first   First view cluster.
 * @param[in]     clusters Number of clusters to fill.
 * @param[in]     seed    Pattern seed.
 */
static void write_orphan_stream(struct block_device *view,
	const struct storage_proxy_lazy_layout *layout, uint32_t first, uint32_t clusters,
	uint32_t seed)
{
	uint8_t sector[512];

	for (uint32_t cluster = 0U; cluster < clusters; cluster++) {
		for (uint32_t index = 0U; index < VIEW_SECTORS_PER_CLUSTER; index++) {
			size_t position = (size_t)cluster * VIEW_CLUSTER_BYTES +
				(size_t)index * 512U;
			uint64_t number = layout->data_start +
				(uint64_t)(first + cluster - 2U) * VIEW_SECTORS_PER_CLUSTER + index;

			for (size_t byte = 0U; byte < 512U; byte++) {
				sector[byte] = pattern_byte(seed, position + byte);
			}
			CHECK(bdev_write(view, number, sector, 1U) == 0);
		}
	}
}

/**
 * @brief Announce a file that owns a data-first stream.
 *
 * @details A free directory sector is located in the first partition folder,
 * a valid exFAT file entry set is placed in its first three slots and the
 * sector is written back. The entry makes the proxy claim the staged stream.
 *
 * @param[in,out] view    Open @c cardview1 block device.
 * @param[in]     layout  View layout.
 * @param[in]     name    File name to create.
 * @param[in]     first   First view cluster of the file.
 * @param[in]     size    Valid file size.
 */
static void write_orphan_entry(struct block_device *view,
	const struct storage_proxy_lazy_layout *layout, const char *name, uint32_t first,
	uint64_t size)
{
	uint8_t sector[512];
	uint64_t number = 0U;
	uint32_t chosen = UINT32_MAX;
	uint32_t slot = 0U;
	uint32_t length = (uint32_t)strlen(name);
	uint8_t *file;
	uint8_t *stream;
	uint8_t *name_entry;
	uint16_t hash = exfat_name_hash(name);
	uint16_t sum;

	CHECK(layout->partition_root_first[0] != 0U);
	CHECK(layout->partition_root_clusters[0] != 0U);
	for (uint32_t sector_index = 0U;
	     sector_index < layout->partition_root_clusters[0] * VIEW_SECTORS_PER_CLUSTER &&
	     chosen == UINT32_MAX; sector_index++) {
		number = layout->data_start +
			(uint64_t)(layout->partition_root_first[0] - 2U) *
			VIEW_SECTORS_PER_CLUSTER + sector_index;
		CHECK(bdev_read(view, number, sector, 1U) == 0);
		for (uint32_t candidate = 0U; candidate + 3U <= 16U; candidate++) {
			bool zero = true;

			for (uint32_t entry = 0U; entry < 3U && zero; entry++) {
				for (uint32_t byte = 0U; byte < 32U; byte++) {
					if (sector[(candidate + entry) * 32U + byte] != 0U) {
						zero = false;
						break;
					}
				}
			}
			if (zero) {
				slot = candidate;
				chosen = sector_index;
				break;
			}
		}
	}
	CHECK(chosen != UINT32_MAX);
	if (chosen == UINT32_MAX) {
		return;
	}
	file = sector + slot * 32U;
	stream = file + 32U;
	name_entry = file + 64U;
	memset(file, 0, 96U);
	file[0] = 0x85U;
	file[1] = 2U;
	store_le(file + 4U, 0x20U, 2U);
	stream[0] = 0xc0U;
	stream[1] = 0x03U;
	stream[3] = (uint8_t)length;
	store_le(stream + 4U, hash, 2U);
	/*
	 * A preallocating host leaves ValidDataLength at zero and puts the file
	 * size in DataLength, exactly like fsutil file createnew. The proxy must
	 * still expose the full size and claim the staged stream.
	 */
	store_le(stream + 8U, 0U, 8U);
	store_le(stream + 20U, first, 4U);
	store_le(stream + 24U, size, 8U);
	name_entry[0] = 0xc1U;
	for (uint32_t index = 0U; index < length; index++) {
		name_entry[2U + index * 2U] = (uint8_t)name[index];
	}
	sum = exfat_set_checksum(file, 3U);
	file[2] = (uint8_t)sum;
	file[3] = (uint8_t)(sum >> 8);
	CHECK(bdev_write(view, number, sector, 1U) == 0);
}

/**
 * @brief Drive the read-write view: create, edit, rename, delete and reload.
 *
 * @details Every operation goes through the mounted @c cardview1, so the host
 * order of a real exFAT driver is used: data clusters are written before the
 * final directory entry. The bulk directory receives enough files to force a
 * host-allocated directory extension cluster. A cache drop then proves the
 * overlay reproduces host-created entries after eviction.
 *
 * @param[in] disk Simulated card.
 */
static void run_write_view(struct gendisk *disk)
{
	struct expected_range edited[4] = {
		{ 0U, 500U, 4100U },
		{ 500U, 300U, 4101U },
		{ 800U, 400U, 4100U },
		{ 1200U, 400U, 4102U },
	};
	struct storage_proxy_lazy_layout layout;
	struct block_device *view;

	(void)disk;
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, 0U) == 0);
	view = bdev_open_by_name("cardview1");
	CHECK(view != NULL);
	if (view == NULL) {
		return;
	}

	/* A new file: the pending table receives the data before the final entry. */
	CHECK(write_pattern("/lazy/partition1/created.bin", 65536U, 4001U, 4096U) == 0);
	CHECK(verify_pattern("/lazy/partition1/created.bin", 65536U, 4001U) == 0);
	CHECK(bdev_flush(view) == 0);

	/* Overwrite, truncate and append a file that already has an extent. */
	CHECK(write_pattern("/lazy/partition1/edit.bin", 2000U, 4100U, 256U) == 0);
	CHECK(write_at("/lazy/partition1/edit.bin", 500U, 300U, 4101U, 128U) == 0);
	{
		struct expected_range ranges[3] = {
			{ 0U, 500U, 4100U },
			{ 500U, 300U, 4101U },
			{ 800U, 1200U, 4100U },
		};

		CHECK(verify_ranges("/lazy/partition1/edit.bin", 2000U, ranges, 3U) == 0);
	}
	CHECK(truncate_file("/lazy/partition1/edit.bin", 1200U) == 0);
	CHECK(append_pattern("/lazy/partition1/edit.bin", 400U, 4102U, 128U) == 0);
	CHECK(verify_ranges("/lazy/partition1/edit.bin", 1600U, edited, 4U) == 0);
	/* A truncating rewrite starts over with the new bytes. */
	CHECK(write_pattern("/lazy/partition1/rewrite.bin", 3000U, 4110U, 512U) == 0);
	CHECK(write_pattern("/lazy/partition1/rewrite.bin", 700U, 4111U, 256U) == 0);
	CHECK(verify_pattern("/lazy/partition1/rewrite.bin", 700U, 4111U) == 0);

	/* Rename a file. */
	CHECK(vfs_rename("/lazy/partition1/created.bin", "/lazy/partition1/moved.bin") == 0);
	CHECK(!directory_has("/lazy/partition1", "created.bin"));
	CHECK(directory_has("/lazy/partition1", "moved.bin"));
	CHECK(verify_pattern("/lazy/partition1/moved.bin", 65536U, 4001U) == 0);

	/* Create, fill and rename a directory. */
	CHECK(vfs_mkdir("/lazy/partition1/newdir") == 0);
	CHECK(write_pattern("/lazy/partition1/newdir/inside.bin", 5000U, 4200U, 512U) == 0);
	CHECK(vfs_rename("/lazy/partition1/newdir", "/lazy/partition1/newdir2") == 0);
	CHECK(directory_has("/lazy/partition1", "newdir2"));
	CHECK(!directory_has("/lazy/partition1", "newdir"));
	CHECK(verify_pattern("/lazy/partition1/newdir2/inside.bin", 5000U, 4200U) == 0);

	/* Delete a file, then a directory after its file. */
	CHECK(write_pattern("/lazy/partition1/victim.bin", 300U, 4300U, 128U) == 0);
	CHECK(vfs_unlink("/lazy/partition1/victim.bin") == 0);
	CHECK(!directory_has("/lazy/partition1", "victim.bin"));
	CHECK(vfs_mkdir("/lazy/partition1/gone") == 0);
	CHECK(write_pattern("/lazy/partition1/gone/old.bin", 300U, 4301U, 128U) == 0);
	CHECK(vfs_unlink("/lazy/partition1/gone/old.bin") == 0);
	CHECK(vfs_rmdir("/lazy/partition1/gone") == 0);
	CHECK(!directory_has("/lazy/partition1", "gone"));

	/* Several hundred files force a multi-cluster view directory. */
	CHECK(vfs_mkdir("/lazy/partition1/bulk") == 0);
	for (uint32_t index = 0U; index < BULK_FILES; index++) {
		char path[64];

		snprintf(path, sizeof(path), "/lazy/partition1/bulk/b%03u.bin", index);
		CHECK(write_pattern(path, 200U + index % 7U, 5000U + index, 128U) == 0);
	}
	CHECK(count_entries("/lazy/partition1/bulk") == BULK_FILES);
	CHECK(verify_pattern("/lazy/partition1/bulk/b000.bin", 200U, 5000U) == 0);
	CHECK(verify_pattern("/lazy/partition1/bulk/b199.bin", 200U + 199U % 7U, 5199U) == 0);
	CHECK(verify_pattern("/lazy/partition1/bulk/b399.bin", 200U + 399U % 7U, 5399U) == 0);
	CHECK(bdev_flush(view) == 0);
	CHECK(storage_proxy_lazy_layout(&layout) == 0);
	{
		uint64_t length = view_entry_length(view, &layout,
			layout.partition_root_first[0], layout.partition_root_clusters[0],
			"bulk");

		/* A host-extended directory advertises more than one 32 KiB cluster. */
		CHECK(length > (uint64_t)VIEW_CLUSTER_BYTES);
	}
	/*
	 * Data that never gets a directory entry: an abandoned write into a free
	 * tail cluster must wait in the pending table and must not touch a source
	 * file. The last tail cluster is far past anything the host allocated.
	 */
	{
		uint8_t sector[512];
		uint32_t cluster = layout.tail_first + layout.tail_clusters - 1U;
		uint64_t number = layout.data_start +
			(uint64_t)(cluster - 2U) * VIEW_SECTORS_PER_CLUSTER;

		memset(sector, 0x5a, sizeof(sector));
		CHECK(bdev_write(view, number, sector, 1U) == 0);
		CHECK(bdev_flush(view) == 0);
	}
	/*
	 * A host that writes megabytes before its final entry: the RAM table holds
	 * the first sectors and the rest must spill to a staging file on the card.
	 * The entry then makes the proxy copy the stream into the source file.
	 */
	{
		struct storage_proxy_lazy_test_stats stats;
		uint32_t first;

		CHECK(storage_proxy_lazy_layout(&layout) == 0);
		CHECK(layout.tail_clusters > 64U + ORPHAN_CLUSTERS);
		first = layout.tail_first + layout.tail_clusters - ORPHAN_CLUSTERS - 64U;
		for (uint32_t cluster = 0U; cluster < ORPHAN_CLUSTERS; cluster++) {
			set_view_bitmap(view, &layout, first + cluster, true);
		}
		write_orphan_stream(view, &layout, first, ORPHAN_CLUSTERS, ORPHAN_SEED);
		CHECK(bdev_flush(view) == 0);
		storage_proxy_lazy_test_stats(&stats);
		CHECK(stats.active_runs == 1U);
		CHECK(stats.staged_sectors >= 1024U);
		write_orphan_entry(view, &layout, "orphan.bin", first,
			(uint64_t)ORPHAN_CLUSTERS * VIEW_CLUSTER_BYTES);
		CHECK(bdev_flush(view) == 0);
		storage_proxy_lazy_test_stats(&stats);
		CHECK(stats.active_runs == 0U);
		CHECK(stats.staged_sectors >= 1024U);
		CHECK(directory_has("/lazy/partition1", "orphan.bin"));
		CHECK(verify_pattern("/lazy/partition1/orphan.bin",
			(uint64_t)ORPHAN_CLUSTERS * VIEW_CLUSTER_BYTES, ORPHAN_SEED) == 0);
	}

	/* Evict every cache and prove the overlay reproduces the host's view. */
	storage_proxy_lazy_cache_drop();
	CHECK(storage_proxy_lazy_cached_entries() == 0U);
	CHECK(directory_has("/lazy/partition1", "moved.bin"));
	CHECK(directory_has("/lazy/partition1", "edit.bin"));
	CHECK(directory_has("/lazy/partition1", "rewrite.bin"));
	CHECK(directory_has("/lazy/partition1", "newdir2"));
	CHECK(directory_has("/lazy/partition1", "bulk"));
	CHECK(!directory_has("/lazy/partition1", "victim.bin"));
	CHECK(!directory_has("/lazy/partition1", "gone"));
	CHECK(verify_pattern("/lazy/partition1/moved.bin", 65536U, 4001U) == 0);
	CHECK(verify_ranges("/lazy/partition1/edit.bin", 1600U, edited, 4U) == 0);
	CHECK(verify_pattern("/lazy/partition1/newdir2/inside.bin", 5000U, 4200U) == 0);
	CHECK(count_entries("/lazy/partition1/bulk") == BULK_FILES);
	CHECK(verify_pattern("/lazy/partition1/bulk/b199.bin", 200U + 199U % 7U, 5199U) == 0);

	CHECK(bdev_flush(view) == 0);
	bdev_close(view);
	CHECK(vfs_umount("/lazy") == 0);
	storage_proxy_lazy_stop();
	CHECK(!storage_proxy_lazy_running());
}

/**
 * @brief Verify through the source files that the host changes reached the card.
 */
static void check_source_writes(void)
{
	struct expected_range edited[4] = {
		{ 0U, 500U, 4100U },
		{ 500U, 300U, 4101U },
		{ 800U, 400U, 4100U },
		{ 1200U, 400U, 4102U },
	};

	CHECK(vfs_mount("mmcblk0p1", "/src1", "fat") == 0);
	/* seed1.bin, many, many2, huge, tree, moved, edit, rewrite, newdir2, bulk, orphan.bin. */
	CHECK(count_entries("/src1") == 11U);
	CHECK(directory_has("/src1", "moved.bin"));
	CHECK(!directory_has("/src1", "created.bin"));
	CHECK(verify_pattern("/src1/moved.bin", 65536U, 4001U) == 0);
	CHECK(verify_pattern("/src1/orphan.bin", (uint64_t)ORPHAN_CLUSTERS * VIEW_CLUSTER_BYTES,
		ORPHAN_SEED) == 0);
	CHECK(!directory_has("/src1", "victim.bin"));
	CHECK(!directory_has("/src1", "gone"));
	CHECK(directory_has("/src1", "newdir2"));
	CHECK(!directory_has("/src1", "newdir"));
	CHECK(verify_pattern("/src1/newdir2/inside.bin", 5000U, 4200U) == 0);
	CHECK(verify_ranges("/src1/edit.bin", 1600U, edited, 4U) == 0);
	CHECK(verify_pattern("/src1/rewrite.bin", 700U, 4111U) == 0);
	CHECK(count_entries("/src1/bulk") == BULK_FILES);
	CHECK(verify_pattern("/src1/bulk/b000.bin", 200U, 5000U) == 0);
	CHECK(verify_pattern("/src1/bulk/b399.bin", 200U + 399U % 7U, 5399U) == 0);
	CHECK(vfs_umount("/src1") == 0);

	CHECK(vfs_mount("mmcblk0p2", "/src2", "ext") == 0);
	/* lost+found, seed2.bin and nested; a leftover staging file would add one. */
	CHECK(count_entries("/src2") == 3U);
	CHECK(verify_pattern("/src2/seed2.bin", 65000U, 2U) == 0);
	CHECK(vfs_umount("/src2") == 0);
}

/**
 * @brief Start the view again after the writes and verify persistence.
 *
 * @param[in] disk Simulated card.
 */
static void run_reload_view(struct gendisk *disk)
{
	(void)disk;
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, 0U) == 0);
	CHECK(directory_has("/lazy/partition1", "moved.bin"));
	CHECK(directory_has("/lazy/partition1", "edit.bin"));
	CHECK(directory_has("/lazy/partition1", "newdir2"));
	CHECK(directory_has("/lazy/partition1", "bulk"));
	CHECK(directory_has("/lazy/partition1", "orphan.bin"));
	CHECK(!directory_has("/lazy/partition1", "victim.bin"));
	CHECK(verify_pattern("/lazy/partition1/moved.bin", 65536U, 4001U) == 0);
	CHECK(verify_pattern("/lazy/partition1/orphan.bin",
		(uint64_t)ORPHAN_CLUSTERS * VIEW_CLUSTER_BYTES, ORPHAN_SEED) == 0);
	CHECK(verify_pattern("/lazy/partition1/newdir2/inside.bin", 5000U, 4200U) == 0);
	CHECK(count_entries("/lazy/partition1/bulk") == BULK_FILES);
	CHECK(verify_pattern("/lazy/partition1/bulk/b199.bin", 200U + 199U % 7U, 5199U) == 0);
	CHECK(vfs_umount("/lazy") == 0);
	storage_proxy_lazy_stop();
}

/**
 * @brief Start the view, mount it, run the checks and stop it again.
 *
 * @details Runs twice so a second start proves the buffers of the first stop
 * are released and the view can be rebuilt.
 *
 * @param[in] disk Simulated card.
 */
static void run_view(struct gendisk *disk)
{
	struct block_device *view;

	(void)disk;
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	CHECK(storage_proxy_lazy_running());
	CHECK(strcmp(storage_proxy_lazy_device_name(), "cardview1") == 0);
	CHECK(storage_proxy_lazy_directory_count() >= 3U);

	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, 0U) == 0);
	view = bdev_open_by_name("cardview1");
	CHECK(view != NULL);
	if (view == NULL) {
		return;
	}
	/* The view is writable now, so the host and the VFS may send writes. */
	CHECK(!bdev_read_only(view));
	check_view_contents(view);
	CHECK(storage_proxy_lazy_cached_entries() > 0U);
	CHECK(storage_proxy_lazy_cached_bytes() > 0U);
	bdev_close(view);
	CHECK(vfs_umount("/lazy") == 0);
	/* A read-only mount of the same view still refuses VFS writes. */
	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, VFS_MOUNT_READ_ONLY) == 0);
	CHECK(vfs_open("/lazy/partition1/forbidden.bin", VFS_O_WRITE | VFS_O_CREATE) < 0);
	CHECK(vfs_umount("/lazy") == 0);
	storage_proxy_lazy_stop();
	CHECK(!storage_proxy_lazy_running());
	CHECK(bdev_open_by_name("cardview1") == NULL);
}

/**
 * @brief First sector of the view FAT, fixed at 32 by lazy_types.h.
 */
#define VIEW_FAT_SECTOR 32U

/** @brief Progress callback invocations seen by the cancellation test. */
static uint32_t progress_calls;

/**
 * @brief Progress callback that cancels the running directory walk.
 *
 * @param[in] context Opaque pointer; ignored.
 * @param[in] entries Entries processed so far; ignored.
 * @param[in] path    Directory path; ignored.
 */
static void cancel_progress(void *context, uint32_t entries, const char *path)
{
	(void)context;
	(void)entries;
	(void)path;
	progress_calls++;
	storage_proxy_lazy_cancel();
}

/**
 * @brief Write one FAT link through the view's block device.
 *
 * @param[in] view    Open @c cardview1 block device.
 * @param[in] cluster Cluster whose entry is written.
 * @param[in] next    Next cluster of the chain.
 * @return 0 on success, or a negative errno.
 */
static int view_fat_link(struct block_device *view, uint32_t cluster, uint32_t next)
{
	uint64_t sector = VIEW_FAT_SECTOR + (uint64_t)(cluster * 4U) / 512U;
	size_t offset = (size_t)(cluster * 4U) % 512U;
	uint8_t buffer[512];

	if (bdev_read(view, sector, buffer, 1U) != 0) {
		return -EIO;
	}
	buffer[offset] = (uint8_t)next;
	buffer[offset + 1U] = (uint8_t)(next >> 8);
	buffer[offset + 2U] = (uint8_t)(next >> 16);
	buffer[offset + 3U] = (uint8_t)(next >> 24);
	return bdev_write(view, sector, buffer, 1U);
}

/**
 * @brief Prove the LRU evicts and reloads directories under a small cache cap.
 *
 * @details A cap just above the large directory lets it fit, so loading a
 * second directory forces an eviction. The evicted directory must then reload
 * with the same slots and extents: its file bytes are checked afterwards.
 */
static void run_cache_eviction(void)
{
	struct block_device *view;

	storage_proxy_lazy_test_limits(1100U, 0U, 0U);
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, 0U) == 0);
	view = bdev_open_by_name("cardview1");
	CHECK(view != NULL);

	storage_proxy_lazy_cache_drop();
	CHECK(count_entries("/lazy/partition1/many") == MANY_FILES);
	CHECK(count_entries("/lazy/partition1/many2") == MANY2_FILES);
	CHECK(storage_proxy_lazy_cached_entries() <= 1100U);
	CHECK(storage_proxy_lazy_cache_evictions() > 0U);
	/* The evicted directory must reload into the same extents. */
	CHECK(verify_pattern("/lazy/partition1/many/f0500.bin",
		100U + 500U % 13U, 1500U) == 0);
	CHECK(verify_pattern("/lazy/partition1/many2/g100.bin", 400U, 6100U) == 0);
	CHECK(storage_proxy_lazy_cached_entries() <= 1100U);

	if (view != NULL) {
		bdev_close(view);
	}
	CHECK(vfs_umount("/lazy") == 0);
	storage_proxy_lazy_stop();
	storage_proxy_lazy_test_limits(0U, 0U, 0U);
}

/**
 * @brief Prove a small directory cap truncates and refuses writes past the span.
 *
 * @details The source holds fifty files, but only the first thirty-two may be
 * visible. The host then links and writes a cluster the view never advertised,
 * which must fail instead of reaching the source directory.
 */
static void run_truncation(void)
{
	struct storage_proxy_lazy_layout layout;
	struct block_device *view;
	uint32_t first = 0U;
	uint32_t clusters = 0U;
	uint64_t length = 0U;

	storage_proxy_lazy_test_limits(0U, 0U, 32U);
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	CHECK(!storage_proxy_lazy_truncated());
	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, 0U) == 0);
	view = bdev_open_by_name("cardview1");
	CHECK(view != NULL);

	CHECK(count_entries("/lazy/partition1/huge") == 32U);
	CHECK(storage_proxy_lazy_truncated());
	check_root_reports(true);
	CHECK(verify_pattern("/lazy/partition1/huge/h0000.bin", 120U, 7000U) == 0);

	if (view != NULL) {
		CHECK(storage_proxy_lazy_layout(&layout) == 0);
		CHECK(view_entry_extent(view, &layout, layout.partition_root_first[0],
			layout.partition_root_clusters[0], "huge", &first, &clusters,
			&length));
		CHECK(first != 0U);
		CHECK(clusters > 0U);
		{
			uint32_t target = layout.tail_first;
			uint64_t target_sector = layout.data_start +
				(uint64_t)(target - 2U) * VIEW_SECTORS_PER_CLUSTER;
			uint32_t extension = first + clusters - 1U;
			uint8_t sector[512];

			memset(sector, 0, sizeof(sector));
			CHECK(view_fat_link(view, extension, target) == 0);
			/* Growing a truncated directory would hide entries; refuse it. */
			CHECK(bdev_write(view, target_sector, sector, 1U) == -ENOSPC);
		}
		CHECK(bdev_flush(view) == 0);
	}
	if (view != NULL) {
		bdev_close(view);
	}
	CHECK(vfs_umount("/lazy") == 0);
	storage_proxy_lazy_stop();
	storage_proxy_lazy_test_limits(0U, 0U, 0U);

	/* The refused write must not have touched the source directory. */
	CHECK(vfs_mount("mmcblk0p1", "/src1", "fat") == 0);
	CHECK(count_entries("/src1/huge") == HUGE_FILES);
	CHECK(verify_pattern("/src1/huge/h0049.bin", 120U + 49U % 9U, 7049U) == 0);
	CHECK(vfs_umount("/src1") == 0);
}

/**
 * @brief Prove a progress callback can cancel a long directory walk.
 *
 * @details Reading the partition folder counts its child directories. The
 * callback cancels at the first progress report, so the block read returns
 * -ECANCELED without caching anything; the view stays running and a later start
 * clears the flag and indexes normally.
 */
static void run_cancel(void)
{
	struct storage_proxy_lazy_layout layout;
	struct block_device *view;
	uint32_t first = 0U;
	uint32_t clusters = 0U;
	uint64_t length = 0U;
	uint8_t sector[512];

	storage_proxy_lazy_test_limits(0U, 0U, 0U);
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	view = bdev_open_by_name("cardview1");
	CHECK(view != NULL);
	if (view == NULL) {
		storage_proxy_lazy_stop();
		return;
	}
	CHECK(storage_proxy_lazy_layout(&layout) == 0);
	CHECK(view_entry_extent(view, &layout, layout.root_cluster, layout.root_clusters,
		"partition1", &first, &clusters, &length));
	CHECK(first != 0U);
	storage_proxy_lazy_cache_drop();

	progress_calls = 0U;
	storage_proxy_lazy_set_progress(cancel_progress, NULL);
	CHECK(bdev_read(view, layout.data_start +
		(uint64_t)(first - 2U) * VIEW_SECTORS_PER_CLUSTER,
		sector, 1U) == -ECANCELED);
	CHECK(progress_calls > 0U);
	CHECK(storage_proxy_lazy_running());
	storage_proxy_lazy_set_progress(NULL, NULL);
	bdev_close(view);
	storage_proxy_lazy_stop();
	CHECK(!storage_proxy_lazy_running());

	/* A fresh start clears the flag and indexes the directory again. */
	CHECK(storage_proxy_lazy_start("mmcblk0", 0x03U) == 0);
	CHECK(vfs_mount_ex("cardview1", "/lazy", NULL, 0U) == 0);
	CHECK(count_entries("/lazy/partition1/many") == MANY_FILES);
	CHECK(vfs_umount("/lazy") == 0);
	storage_proxy_lazy_stop();
	CHECK(!storage_proxy_lazy_running());
}

/**
 * @brief Run the lazy proxy host test.
 *
 * @return 0 when every check passed, 1 otherwise.
 */
int main(void)
{
	struct gendisk *disk;

	disk_data = aligned_alloc(64U, (size_t)CARD_SECTORS * 512U);
	CHECK(disk_data != NULL);
	CHECK(vfs_ff_register() == 0);
	CHECK(vfs_ext_register() == 0);
	build_card(&disk);

	run_view(disk);
	/* A second cycle proves stop released the caches and the sources. */
	run_view(disk);

	/* Runtime caps, truncation and cancellation, all read-only on the card. */
	run_cache_eviction();
	run_truncation();
	run_cancel();

	/* Write through the view, verify the source, then reload the view. */
	run_write_view(disk);
	check_source_writes();
	run_reload_view(disk);

	/* The source card still holds every file it was built with. */
	CHECK(vfs_mount("mmcblk0p1", "/src1", "fat") == 0);
	CHECK(count_entries("/src1/many") == MANY_FILES);
	CHECK(vfs_umount("/src1") == 0);

	CHECK(del_gendisk(disk) == 0);
	put_disk(disk);
	free(disk_data);
	printf("lazy proxy test: %s (%u checks, %u failures)\n",
		failures == 0U ? "PASS" : "FAIL", checks, failures);
	return failures == 0U ? 0 : 1;
}
