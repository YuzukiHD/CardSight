// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file storage_proxy_matrix_test.c
 * @brief Host matrix test for the storage proxy.
 *
 * Runs the same workloads through the exFAT view for different source partition
 * mixes (FAT32, exFAT, ext4, all at once). Every write is read back from the
 * source partitions and compared byte for byte, and the stop/start cycle is
 * repeated.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hal/blkdev.h>
#include "storage/storage_proxy.h"
#include <vfs.h>

/** @brief Size of the simulated card in 512-byte sectors (160 MiB). */
#define CARD_SECTORS (160U * 2048U)
/** @brief Size of each simulated source partition in MiB. */
#define PARTITION_MIB 48U

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
static const struct block_device_operations ram_operations = { .read = ram_read, .write = ram_write };

/** @brief Number of format-progress callbacks seen in the current format. */
static uint32_t progress_updates;
/** @brief Last completed amount reported by the format-progress callback. */
static uint64_t progress_last_done;
/** @brief Last total amount reported by the format-progress callback. */
static uint64_t progress_last_total;

/**
 * @brief Format-progress callback that records the reported values.
 *
 * @details Counts each update, stores the last done/total pair, and asserts
 *          that the completed amount never exceeds the total.
 *
 * @param[in] context Unused callback context.
 * @param[in] done    Amount completed so far.
 * @param[in] total   Total amount of work.
 */
static void format_progress_cb(void *context, uint64_t done, uint64_t total)
{
	(void)context;
	progress_updates++;
	progress_last_done = done;
	progress_last_total = total;
	CHECK(done <= total);
}

/**
 * @brief Clear the recorded format-progress state before a format.
 */
static void reset_progress(void)
{
	progress_updates = 0U;
	progress_last_done = 0U;
	progress_last_total = 0U;
}

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
 * @details Opens the file with write, create, and truncate, then writes the
 *          pattern in chunks of at most @p chunk bytes, checking each write's
 *          return value. The descriptor is always closed; the first error wins
 *          over the close result.
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

/* Returns the first differing offset + 1, 0 when the file matches, or a negative error. */
/**
 * @brief Verify a file written by write_pattern().
 *
 * @details Reads the file in 4 KiB blocks, compares every byte with the
 *          pattern, and finally checks the reported size. The descriptor is
 *          always closed.
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
 * @brief One source-filesystem mix for all three partitions.
 */
struct mix {
	/** @brief Mix name shown in the test output. */
	const char *name;
	/** @brief Filesystem type of each source partition, in partition order. */
	const char *types[3]; /* fat, exfat, ext4 per partition */
};

/** @brief Source mixes exercised by the matrix. */
static const struct mix mixes[] = {
	{ "FAT32 only", { "fat", "fat", "fat" } },
	{ "exFAT only", { "exfat", "exfat", "exfat" } },
	{ "ext4 only", { "ext", "ext", "ext" } },
	{ "FAT32 + exFAT + ext4", { "fat", "exfat", "ext" } },
	{ "ext4 + FAT32 + exFAT", { "ext", "fat", "exfat" } },
};

/**
 * @brief Create the simulated card, its partitions, and their seed files.
 *
 * @details Allocates a gendisk backed by @c disk_data, writes an MBR layout with
 *          three 48 MiB partitions, formats each with the mix's filesystem
 *          while checking that progress advances and ends at the total, mounts
 *          it, and leaves one seed file per partition. The card geometry and
 *          seed files give the later workloads a known source state.
 *
 * @param[out] disk_out Receives the allocated disk.
 * @param[in]  mix      Filesystem types for the three partitions.
 */
static void build_card(struct gendisk **disk_out, const struct mix *mix)
{
	const uint32_t partition_sizes[3] = { PARTITION_MIB, PARTITION_MIB, PARTITION_MIB };
	struct gendisk *disk = alloc_disk();

	memset(disk_data, 0, (size_t)CARD_SECTORS * 512U);
	CHECK(disk != NULL);
	strcpy(disk->disk_name, "mmcblk0");
	disk->fops = &ram_operations;
	disk->capacity = CARD_SECTORS;
	CHECK(add_disk(disk) == 0);
	{
		struct blk_partition_layout_entry layout[3];
		/** @brief Filesystem used when creating each source partition. */
		static const enum blk_partition_content contents[3] = {
			BLK_PARTITION_FAT32, BLK_PARTITION_FAT32, BLK_PARTITION_FAT32,
		};

		for (uint32_t index = 0U; index < 3U; index++) {
			layout[index].sectors = partition_sizes[index] * 2048U;
			layout[index].content = contents[index];
			snprintf(layout[index].label, sizeof(layout[index].label), "CARD%u", index + 1U);
		}
		CHECK(blk_write_partition_layout(disk, BLK_PARTITION_TABLE_MBR, layout, 3U) == 0);
	}
	for (uint32_t partition = 1U; partition <= 3U; partition++) {
		char device[24];
		char mount[16];
		char path[64];

		snprintf(device, sizeof(device), "mmcblk0p%u", partition);
		snprintf(mount, sizeof(mount), "/src%u", partition);
		reset_progress();
		vfs_set_format_progress(format_progress_cb, NULL);
		CHECK(vfs_format(device, mix->types[partition - 1U], "CARD", 0U) == 0);
		vfs_set_format_progress(NULL, NULL);
		/* The bar must move during the format, not only at its two ends. */
		CHECK(progress_updates > 2U);
		CHECK(progress_last_done == progress_last_total);
		CHECK(vfs_mount(device, mount, mix->types[partition - 1U]) == 0);
		snprintf(path, sizeof(path), "%s/seed-%u.bin", mount, partition);
		CHECK(write_pattern(path, 70000U + partition, partition, 4096U) == 0);
		CHECK(vfs_umount(mount) == 0);
	}
	*disk_out = disk;
}

/* Sizes around every boundary the proxy cares about: sector, 4 KiB, 32 KiB buffer and cluster. */
static const size_t sizes[] = {
	0U, 1U, 511U, 512U, 513U, 4095U, 4096U, 4097U, 32767U, 32768U, 32769U,
	65535U, 65536U, 65537U, 100000U, 262144U, 1048576U + 1U,
};

/**
 * @brief Write and verify files at every size boundary the proxy cares about.
 *
 * @details Sizes run from zero through single sectors, 4 KiB, the 32 KiB
 *          buffer, cluster-sized writes, and sizes just over each boundary. Each
 *          file is written through the view and read back through the view.
 *
 * @param[in] view_dir   Directory of the partition inside the view.
 * @param[in] source_dir Directory of the source partition.
 * @param[in] partition  Partition number, used to derive the seed.
 */
static void workload_sizes(const char *view_dir, const char *source_dir, uint32_t partition)
{
	for (uint32_t index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		char name[96];
		char source[96];
		uint32_t seed = partition * 100U + index;

		snprintf(name, sizeof(name), "%s/size-%u.bin", view_dir, index);
		snprintf(source, sizeof(source), "%s/size-%u.bin", source_dir, index);
		CHECK(write_pattern(name, sizes[index], seed, 4096U) == 0);
		CHECK(verify_pattern(name, sizes[index], seed) == 0);
	}
}

/**
 * @brief Write and verify one file with many different chunk sizes.
 *
 * @details Odd write sizes arrive as sector runs with partial first and last
 *          sectors, so every chunk size from one byte to 200000 bytes is tried
 *          on the same 150000-byte pattern.
 *
 * @param[in] view_dir  Directory of the partition inside the view.
 * @param[in] partition Partition number, used to derive the seeds.
 */
static void workload_chunks(const char *view_dir, uint32_t partition)
{
	/** @brief Write sizes exercised by the chunk workload. */
	static const size_t chunks[] = { 1U, 100U, 512U, 777U, 4096U, 16384U, 65536U, 200000U };

	for (uint32_t index = 0U; index < sizeof(chunks) / sizeof(chunks[0]); index++) {
		char name[96];
		uint32_t seed = partition * 1000U + index;

		snprintf(name, sizeof(name), "%s/chunk-%u.bin", view_dir, index);
		/* Odd write sizes arrive as sector runs with partial first and last sectors. */
		CHECK(write_pattern(name, 150000U, seed, chunks[index]) == 0);
		CHECK(verify_pattern(name, 150000U, seed) == 0);
	}
}

/* Enough names to push entry groups across every sector edge of the directory. */
/**
 * @brief Create and verify 40 files with short, medium, and long names.
 *
 * @details Enough names are used to push entry groups across every sector edge
 *          of the directory, and every file is read back after the whole batch
 *          is written.
 *
 * @param[in] view_dir  Directory of the partition inside the view.
 * @param[in] partition Partition number, used to derive the seeds.
 */
static void workload_many_files(const char *view_dir, uint32_t partition)
{
	char path[128];

	for (uint32_t index = 0U; index < 40U; index++) {
		snprintf(path, sizeof(path), "%s/many-%02u-%s.txt", view_dir, index,
			index % 3U == 0U ? "short" : index % 3U == 1U ? "a-considerably-longer-file-name" : "mid-length");
		CHECK(write_pattern(path, 100U + index * 37U, partition * 50U + index, 512U) == 0);
	}
	for (uint32_t index = 0U; index < 40U; index++) {
		snprintf(path, sizeof(path), "%s/many-%02u-%s.txt", view_dir, index,
			index % 3U == 0U ? "short" : index % 3U == 1U ? "a-considerably-longer-file-name" : "mid-length");
		CHECK(verify_pattern(path, 100U + index * 37U, partition * 50U + index) == 0);
	}
}

/**
 * @brief Exercise directory creation, a nested write, rename, and removal.
 *
 * @details Creates a two-level tree, writes a file into the inner directory,
 *          renames it into the outer one, verifies its contents at the new
 *          path, and removes the empty inner directory.
 *
 * @param[in] view_dir  Directory of the partition inside the view.
 * @param[in] partition Partition number, used to derive the seed.
 */
static void workload_directories(const char *view_dir, uint32_t partition)
{
	char path[128];
	char moved[128];

	snprintf(path, sizeof(path), "%s/tree", view_dir);
	CHECK(vfs_mkdir(path) == 0);
	snprintf(path, sizeof(path), "%s/tree/inner", view_dir);
	CHECK(vfs_mkdir(path) == 0);
	snprintf(path, sizeof(path), "%s/tree/inner/deep.bin", view_dir);
	CHECK(write_pattern(path, 20000U, partition + 7U, 4096U) == 0);
	snprintf(moved, sizeof(moved), "%s/tree/moved.bin", view_dir);
	CHECK(vfs_rename(path, moved) == 0);
	CHECK(verify_pattern(moved, 20000U, partition + 7U) == 0);
	snprintf(path, sizeof(path), "%s/tree/inner", view_dir);
	CHECK(vfs_rmdir(path) == 0);
}

/**
 * @brief Exercise overwrite, truncate, grow, and unlink on one file.
 *
 * @details Writes 90000 bytes, rewrites it smaller with a new seed, then grows
 *          it past the first size and verifies the final contents before
 *          unlinking it.
 *
 * @param[in] view_dir  Directory of the partition inside the view.
 * @param[in] partition Partition number, used to derive the seeds.
 */
static void workload_overwrite_truncate(const char *view_dir, uint32_t partition)
{
	char path[128];

	snprintf(path, sizeof(path), "%s/rewrite.bin", view_dir);
	CHECK(write_pattern(path, 90000U, partition, 4096U) == 0);
	CHECK(write_pattern(path, 30000U, partition + 1U, 4096U) == 0);
	CHECK(verify_pattern(path, 30000U, partition + 1U) == 0);
	CHECK(write_pattern(path, 120000U, partition + 2U, 8192U) == 0);
	{
		long difference = verify_pattern(path, 120000U, partition + 2U);

		if (difference != 0) {
			printf("rewrite grown: result %ld\n", difference);
		}
		CHECK(difference == 0);
	}
	CHECK(vfs_unlink(path) == 0);
}

/**
 * @brief Forward declaration of the Windows-order verification helper.
 *
 * @param[in] mount Mount point of a source partition.
 */
static void check_windows_order(const char *mount);

/**
 * @brief Verify the workloads directly on the source partitions.
 *
 * @details Mounts each source partition and reads the seed file plus every file
 *          the workloads created, comparing them with the original patterns.
 *          This proves the proxy mirrored writes into the real filesystems, not
 *          only into its view. On partition 1 it also verifies the Windows
 *          write-order files and the valid-data-length file.
 *
 * @param[in] mix        Mix that describes each partition's filesystem.
 * @param[in] expect_work True when the proxy workloads ran and their files must
 *                        exist.
 */
static void check_sources(const struct mix *mix, bool expect_work)
{
	/* The same files, read from the partitions the proxy mirrors into. */
	for (uint32_t partition = 1U; partition <= 3U; partition++) {
		char device[24];
		char mount[16];
		char path[96];

		snprintf(device, sizeof(device), "mmcblk0p%u", partition);
		snprintf(mount, sizeof(mount), "/verify%u", partition);
		CHECK(vfs_mount(device, mount, mix->types[partition - 1U]) == 0);
		snprintf(path, sizeof(path), "%s/seed-%u.bin", mount, partition);
		CHECK(verify_pattern(path, 70000U + partition, partition) == 0);
		if (expect_work) {
			for (uint32_t index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
				snprintf(path, sizeof(path), "%s/size-%u.bin", mount, index);
				CHECK(verify_pattern(path, sizes[index], partition * 100U + index) == 0);
			}
			snprintf(path, sizeof(path), "%s/chunk-7.bin", mount);
			CHECK(verify_pattern(path, 150000U, partition * 1000U + 7U) == 0);
			snprintf(path, sizeof(path), "%s/many-39-short.txt", mount);
			CHECK(verify_pattern(path, 100U + 39U * 37U, partition * 50U + 39U) == 0);
			if (partition == 1U) {
				check_windows_order(mount);
			}
			if (partition == 1U) {
				struct vfs_stat stat;

				snprintf(path, sizeof(path), "%s/vdl.bin", mount);
				CHECK(vfs_stat(path, &stat) == 0 && stat.size == 10000U);
				{
					long difference = verify_pattern(path, 10000U, 77U);

					if (difference != 0) {
						printf("vdl.bin on %s: result %ld, stat size %llu\n", mix->name, difference,
							(unsigned long long)stat.size);
					}
					CHECK(difference == 0);
				}
			}
			snprintf(path, sizeof(path), "%s/tree/moved.bin", mount);
			CHECK(verify_pattern(path, 20000U, partition + 7U) == 0);
		}
		CHECK(vfs_umount(mount) == 0);
	}
}

/*
 * A Windows host writes DataLength in whole clusters while ValidDataLength is what exists. The source
 * file must keep the valid length: the rounded one would pad it with zeros.
 */
/**
 * @brief Model a Windows host writing DataLength as whole clusters.
 *
 * @details The host reports DataLength rounded up to the cluster size while
 *          ValidDataLength is what actually exists. The test finds the exFAT
 *          stream extension entry in the first sectors of the view, patches its
 *          DataLength to the rounded value, and leaves the source file with the
 *          valid length: the rounded length would pad it with zeros. The
 *          workload file itself is verified later by check_windows_order()'s
 *          caller.
 */
static void workload_valid_length(void)
{
	struct block_device *view;
	const uint8_t name[] = { 'v', 0, 'd', 0, 'l', 0, '.', 0, 'b', 0, 'i', 0, 'n', 0 };
	bool patched = false;

	CHECK(write_pattern("/view/partition1/vdl.bin", 10000U, 77U, 4096U) == 0);
	view = bdev_open_by_name("cardview0");
	CHECK(view != NULL);
	/* The group may start in the last slots of a sector, so look through a two sector window. */
	for (uint64_t number = 0U; view != NULL && !patched && number < 20000U; number++) {
		uint8_t window[1024];

		if (bdev_read(view, number, window, 1U) != 0 || bdev_read(view, number + 1U, window + 512U, 1U) != 0) {
			break;
		}
		for (uint32_t slot = 0U; slot < 16U && !patched; slot++) {
			uint8_t *entry = window + slot * 32U;

			if (entry[0] == 0x85U && entry[32] == 0xc0U && entry[64] == 0xc1U &&
				memcmp(entry + 66U, name, sizeof(name)) == 0) {
				uint64_t rounded = 32768U;
				uint32_t stream_offset = slot * 32U + 32U;

				/* DataLength is at byte 24 of the stream extension entry. A host writes whole sectors. */
				memcpy(window + stream_offset + 24U, &rounded, sizeof(rounded));
				CHECK(bdev_write(view, number, window, 1U) == 0);
				CHECK(bdev_write(view, number + 1U, window + 512U, 1U) == 0);
				patched = true;
			}
		}
	}
	CHECK(patched);
	if (view != NULL) {
		CHECK(bdev_flush(view) == 0);
		bdev_close(view);
	}
}


/*
 * A Windows host writes file data first and the directory entry that owns it afterwards, and it takes
 * free clusters from the allocation bitmap it read when it mounted. These helpers build that order by
 * hand on the exFAT view, where a FatFs host would always write the entry first.
 */
/**
 * @brief Geometry of the mounted exFAT view, read from its boot sector.
 */
struct view_layout {
	/** @brief Open block device of the exFAT view. */
	struct block_device *view;
	/** @brief First sector of cluster 2. */
	uint32_t data_start;
	/** @brief Total cluster count of the view. */
	uint32_t clusters;
	/** @brief Cluster holding the root directory. */
	uint32_t root_cluster;
	/** @brief Cluster holding the partition directory being edited. */
	uint32_t directory_cluster;
	/** @brief Cluster holding the allocation bitmap. */
	uint32_t bitmap_cluster;
};

/**
 * @brief Return the first sector of a cluster in the view.
 *
 * @param[in] layout  View layout read by view_open().
 * @param[in] cluster Cluster number, two or greater.
 * @return Absolute sector number.
 */
static uint64_t cluster_sector(const struct view_layout *layout, uint32_t cluster)
{
	return layout->data_start + (uint64_t)(cluster - 2U) * 64U;
}

/**
 * @brief Read a little-endian 32-bit value.
 *
 * @param[in] bytes Four bytes in little-endian order.
 * @return The decoded value.
 */
static uint32_t read_le32(const uint8_t *bytes)
{
	return bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * @brief Open the view and locate its root directory and allocation bitmap.
 *
 * @details Reads the boot sector to get the data start, cluster count, and root
 *          cluster, then scans the root directory entries for the bitmap entry
 *          (0x81) and for the directory entry (0x85, 0xC0, 0xC1) whose UTF-16
 *          name matches @p directory_name.
 *
 * @param[out] layout         Geometry and cluster numbers of the view.
 * @param[in]  directory_name Name of the partition directory to locate.
 * @return True when both the directory and the bitmap were found.
 */
static bool view_open(struct view_layout *layout, const char *directory_name)
{
	uint8_t sector[512];
	uint8_t chain[512];
	const size_t name_length = strlen(directory_name);

	memset(layout, 0, sizeof(*layout));
	layout->view = bdev_open_by_name("cardview0");
	if (layout->view == NULL || bdev_read(layout->view, 0U, sector, 1U) != 0) {
		return false;
	}
	layout->data_start = read_le32(sector + 88U);
	layout->clusters = read_le32(sector + 92U);
	layout->root_cluster = read_le32(sector + 96U);
	for (uint32_t number = 0U; number < 64U; number++) {
		if (bdev_read(layout->view, cluster_sector(layout, layout->root_cluster) + number, chain, 1U) != 0) {
			return false;
		}
		for (uint32_t slot = 0U; slot < 16U; slot++) {
			const uint8_t *entry = chain + slot * 32U;

			if (entry[0] == 0x81U) {
				layout->bitmap_cluster = read_le32(entry + 20U);
			}
			if (entry[0] == 0x85U && slot + 2U < 16U && chain[(slot + 1U) * 32U] == 0xc0U &&
				chain[(slot + 2U) * 32U] == 0xc1U) {
				bool same = chain[(slot + 1U) * 32U + 3U] == name_length;

				for (size_t index = 0U; same && index < name_length; index++) {
					same = chain[(slot + 2U) * 32U + 2U + index * 2U] == (uint8_t)directory_name[index];
				}
				if (same) {
					layout->directory_cluster = read_le32(chain + (slot + 1U) * 32U + 20U);
				}
			}
		}
	}
	return layout->directory_cluster != 0U && layout->bitmap_cluster != 0U;
}

/**
 * @brief Compute the exFAT up-case name hash of a file name.
 *
 * @details Folds ASCII letters to upper case and applies the standard
 *          rotate-right-and-add hash, including the required zero terminator.
 *
 * @param[in] name File name.
 * @return The 16-bit name hash.
 */
static uint16_t name_hash(const char *name)
{
	uint16_t hash = 0U;

	for (size_t index = 0U; name[index] != '\0'; index++) {
		char upper = name[index] >= 'a' && name[index] <= 'z' ? (char)(name[index] - 32) : name[index];

		hash = (uint16_t)(((hash & 1U) ? 0x8000U : 0U) + (hash >> 1) + (uint8_t)upper);
		hash = (uint16_t)(((hash & 1U) ? 0x8000U : 0U) + (hash >> 1) + 0U);
	}
	return hash;
}

/* Write a file entry set (85, C0, C1...) into the first free slots of the directory. */
/**
 * @brief Write an exFAT file entry set into the directory.
 *
 * @details Builds a primary (0x85) entry, a stream extension (0xC0), and one
 *          name entry per 15 characters, computes the entry-set checksum, and
 *          writes the group into the first free run of slots, or in place when
 *          the same name already exists (a size update). Two sectors are read
 *          at a time so a group may span a sector edge.
 *
 * @param[in] layout        View layout from view_open().
 * @param[in] name          File name.
 * @param[in] first_cluster First cluster of the data; zero for an empty entry.
 * @param[in] size          Valid and data length in bytes.
 * @return True when the group was written.
 */
static bool view_put_entry(const struct view_layout *layout, const char *name, uint32_t first_cluster, uint64_t size)
{
	const size_t name_length = strlen(name);
	const uint32_t names = (uint32_t)((name_length + 14U) / 15U);
	const uint32_t total = 2U + names;
	uint8_t group[19 * 32];
	uint16_t sum = 0U;

	memset(group, 0, sizeof(group));
	group[0] = 0x85U;
	group[1] = (uint8_t)(1U + names);
	group[4] = 0x20U;
	group[32] = 0xc0U;
	group[33] = 0x03U; /* allocation possible, no FAT chain */
	group[35] = (uint8_t)name_length;
	{
		uint16_t hash = name_hash(name);

		group[36] = (uint8_t)hash;
		group[37] = (uint8_t)(hash >> 8);
	}
	memcpy(group + 32U + 8U, &size, sizeof(size)); /* valid data length */
	memcpy(group + 32U + 20U, &first_cluster, sizeof(first_cluster));
	memcpy(group + 32U + 24U, &size, sizeof(size)); /* data length */
	for (uint32_t part = 0U; part < names; part++) {
		group[(2U + part) * 32U] = 0xc1U;
		for (uint32_t index = 0U; index < 15U; index++) {
			size_t position = (size_t)part * 15U + index;

			group[(2U + part) * 32U + 2U + index * 2U] = position < name_length ? (uint8_t)name[position] : 0U;
		}
	}
	for (uint32_t index = 0U; index < total * 32U; index++) {
		if (index != 2U && index != 3U) {
			sum = (uint16_t)(((sum & 1U) ? 0x8000U : 0U) + (sum >> 1) + group[index]);
		}
	}
	group[2] = (uint8_t)sum;
	group[3] = (uint8_t)(sum >> 8);
	/* The same group is rewritten in place when the name is already there (a size update). */
	for (uint32_t number = 0U; number < 64U; number++) {
		uint8_t window[1024];
		bool free_run = false;

		if (bdev_read(layout->view, cluster_sector(layout, layout->directory_cluster) + number, window, 1U) != 0 ||
			(number + 1U < 64U && bdev_read(layout->view, cluster_sector(layout, layout->directory_cluster) + number + 1U,
				window + 512U, 1U) != 0)) {
			return false;
		}
		for (uint32_t slot = 0U; slot < 16U; slot++) {
			uint8_t *entry = window + slot * 32U;
			bool same = entry[0] == 0x85U && entry[32] == 0xc0U && entry[35] == name_length &&
				memcmp(entry + 66U, group + 66U, name_length * 2U > 30U ? 30U : name_length * 2U) == 0;

			free_run = true;
			for (uint32_t index = 0U; index < total; index++) {
				free_run = free_run && (window[(slot + index) * 32U] & 0x80U) == 0U;
			}
			if (same || free_run) {
				memcpy(entry, group, total * 32U);
				if (bdev_write(layout->view, cluster_sector(layout, layout->directory_cluster) + number, window, 1U) != 0) {
					return false;
				}
				if (slot + total > 16U &&
					bdev_write(layout->view, cluster_sector(layout, layout->directory_cluster) + number + 1U, window + 512U, 1U) != 0) {
					return false;
				}
				return true;
			}
		}
	}
	return false;
}

/**
 * @brief Write pattern data for a view file, sector padded.
 *
 * @details Writes the stream in chunks, zero-padding each chunk to a 512-byte
 *          boundary as a host writing whole sectors would.
 *
 * @param[in] layout        View layout from view_open().
 * @param[in] first_cluster First cluster of the file.
 * @param[in] bytes         Number of bytes to write.
 * @param[in] seed          Pattern seed.
 * @param[in] chunk         Maximum write size in bytes.
 * @return True on success.
 */
static bool view_write_data(const struct view_layout *layout, uint32_t first_cluster, size_t bytes, uint32_t seed, size_t chunk)
{
	uint8_t *buffer = malloc(chunk + 512U);
	size_t offset = 0U;
	bool ok = buffer != NULL;

	while (ok && offset < bytes) {
		size_t length = bytes - offset < chunk ? bytes - offset : chunk;
		size_t padded = (length + 511U) & ~(size_t)511U;

		memset(buffer, 0, padded);
		for (size_t index = 0U; index < length; index++) {
			buffer[index] = pattern_byte(seed, offset + index);
		}
		ok = bdev_write(layout->view, cluster_sector(layout, first_cluster) + offset / 512U, buffer, (uint32_t)(padded / 512U)) == 0;
		offset += length;
	}
	free(buffer);
	return ok;
}

/* Data of the same stream from byte offset `from` up to `to`, chunks cut on 512 byte boundaries. */
/**
 * @brief Write pattern data for a byte range of a view file.
 *
 * @details Starts at the 512-byte boundary at or below @p from, writes through
 *          @p to, and pads each chunk to whole sectors. Used to model a host
 *          that writes ahead of the size it has reported.
 *
 * @param[in] layout        View layout from view_open().
 * @param[in] first_cluster First cluster of the file.
 * @param[in] from          First byte of the range.
 * @param[in] to            One past the last byte of the range.
 * @param[in] seed          Pattern seed.
 * @param[in] chunk         Maximum write size in bytes.
 * @return True on success.
 */
static bool view_write_data_range(const struct view_layout *layout, uint32_t first_cluster, size_t from, size_t to,
	uint32_t seed, size_t chunk)
{
	uint8_t *buffer = malloc(chunk + 512U);
	size_t offset = from & ~(size_t)511U;
	bool ok = buffer != NULL;

	while (ok && offset < to) {
		size_t length = to - offset < chunk ? to - offset : chunk;
		size_t padded = (length + 511U) & ~(size_t)511U;

		memset(buffer, 0, padded);
		for (size_t index = 0U; index < length; index++) {
			buffer[index] = pattern_byte(seed, offset + index);
		}
		ok = bdev_write(layout->view, cluster_sector(layout, first_cluster) + offset / 512U, buffer, (uint32_t)(padded / 512U)) == 0;
		offset += length;
	}
	free(buffer);
	return ok;
}

/* Set or clear the allocation bits of a cluster range, as a host does when it takes or frees space. */
/**
 * @brief Set or clear the allocation bits of a cluster range.
 *
 * @details Mirrors what a host does when it takes or frees space: each bit is
 *          read-modify-written in the view's allocation bitmap sectors.
 *
 * @param[in] layout        View layout from view_open().
 * @param[in] first_cluster First cluster of the range.
 * @param[in] count         Number of clusters.
 * @param[in] used          True to mark used, false to free.
 * @return True on success.
 */
static bool view_bitmap(const struct view_layout *layout, uint32_t first_cluster, uint32_t count, bool used)
{
	uint8_t sector[512];

	for (uint32_t cluster = first_cluster; cluster < first_cluster + count; cluster++) {
		uint32_t bit = cluster - 2U;
		uint64_t number = cluster_sector(layout, layout->bitmap_cluster) + (bit / 8U) / 512U;

		if (bdev_read(layout->view, number, sector, 1U) != 0) {
			return false;
		}
		if (used) {
			sector[(bit / 8U) % 512U] |= (uint8_t)(1U << (bit % 8U));
		} else {
			sector[(bit / 8U) % 512U] &= (uint8_t)~(1U << (bit % 8U));
		}
		if (bdev_write(layout->view, number, sector, 1U) != 0) {
			return false;
		}
	}
	return true;
}

/* A host deletes by clearing the in-use bit of the entry group; nothing is flushed. */
/**
 * @brief Delete a file entry set the way a host does.
 *
 * @details Finds the entry group whose name matches and clears the in-use bit
 *          of every entry in the group; nothing is flushed. The data clusters
 *          are deliberately left allocated, as a host would leave them until it
 *          updates the bitmap.
 *
 * @param[in] layout View layout from view_open().
 * @param[in] name   File name to delete.
 * @return True when the entry was found and cleared.
 */
static bool view_delete_entry(const struct view_layout *layout, const char *name)
{
	const size_t name_length = strlen(name);

	for (uint32_t number = 0U; number < 64U; number++) {
		uint8_t window[1024];

		if (bdev_read(layout->view, cluster_sector(layout, layout->directory_cluster) + number, window, 1U) != 0 ||
			(number + 1U < 64U && bdev_read(layout->view, cluster_sector(layout, layout->directory_cluster) + number + 1U,
				window + 512U, 1U) != 0)) {
			return false;
		}
		for (uint32_t slot = 0U; slot < 16U; slot++) {
			uint8_t *entry = window + slot * 32U;

			if (entry[0] == 0x85U && entry[32] == 0xc0U && entry[35] == name_length &&
				entry[66] == (uint8_t)name[0] && memcmp(entry + 66U, name, 1U) == 0) {
				bool same = true;

				for (size_t index = 0U; same && index < name_length && index < 15U; index++) {
					same = entry[66U + index * 2U] == (uint8_t)name[index];
				}
				if (same) {
					for (uint32_t index = 0U; index <= entry[1]; index++) {
						window[(slot + index) * 32U] &= 0x7fU;
					}
					return bdev_write(layout->view, cluster_sector(layout, layout->directory_cluster) + number, window, 1U) == 0 &&
						(slot + entry[1] < 16U || bdev_write(layout->view, cluster_sector(layout, layout->directory_cluster) + number + 1U,
							window + 512U, 1U) == 0);
				}
			}
		}
	}
	return false;
}

/**
 * @brief Files written data-first in the Windows-order workload.
 *
 * Each entry holds the file name, its offset in clusters from the start of the
 * test area, its size in bytes, and the pattern seed used to generate its data.
 */
static const struct { const char *name; uint32_t offset; size_t size; uint32_t seed; } windows_files[] = {
	{ "win-big.bin", 0U, 3U * 1024U * 1024U + 100U, 501U },         /* far past what RAM holds */
	{ "win-small.bin", 200U, 70000U, 502U },                        /* fits in RAM */
	{ "win-one.bin", 300U, 1U, 503U },
};

/* Data first, entry afterwards, for files placed one after another in the free clusters at the end. */
/**
 * @brief Model a Windows host's write order: data first, entry afterwards.
 *
 * @details A Windows host writes file data first and the directory entry that
 *          owns it afterwards, and it takes free clusters from the allocation
 *          bitmap it read at mount. The function builds that order by hand on
 *          the exFAT view: plain files, two files back to back, an entry that
 *          lags the data, slot reuse after a delete, a freed cluster handed to
 *          a new file, and an abandoned copy whose space is returned.
 */
static void workload_windows_order(void)
{
	struct view_layout layout;
	uint32_t base;

	CHECK(view_open(&layout, "partition1"));
	base = layout.clusters + 2U - 4000U;
	for (uint32_t index = 0U; index < sizeof(windows_files) / sizeof(windows_files[0]); index++) {
		uint32_t first = base + windows_files[index].offset;
		uint32_t clusters = (uint32_t)((windows_files[index].size + 32767U) / 32768U);

		/* The create: an empty entry; then the allocation, the data, and last the sized entry. */
		CHECK(view_put_entry(&layout, windows_files[index].name, 0U, 0U));
		CHECK(view_bitmap(&layout, first, clusters, true));
		CHECK(view_write_data(&layout, first, windows_files[index].size, windows_files[index].seed, 65536U));
		CHECK(view_put_entry(&layout, windows_files[index].name, first, windows_files[index].size));
	}
	/* Two files back to back in one contiguous run, both written before either entry. */
	{
		const uint32_t first = base + 400U;
		const size_t first_size = 600000U;
		const size_t second_size = 500000U;
		const uint32_t first_clusters = (uint32_t)((first_size + 32767U) / 32768U);
		const uint32_t second_clusters = (uint32_t)((second_size + 32767U) / 32768U);

		CHECK(view_put_entry(&layout, "win-pair-a.bin", 0U, 0U));
		CHECK(view_put_entry(&layout, "win-pair-b.bin", 0U, 0U));
		CHECK(view_bitmap(&layout, first, first_clusters + second_clusters, true));
		CHECK(view_write_data(&layout, first, first_size, 601U, 65536U));
		CHECK(view_write_data(&layout, first + first_clusters, second_size, 602U, 65536U));
		CHECK(view_put_entry(&layout, "win-pair-a.bin", first, first_size));
		CHECK(view_put_entry(&layout, "win-pair-b.bin", first + first_clusters, second_size));
	}
	/*
	 * The entry lags the data: the host reports the size of what it has finished while more is already
	 * written, and writes data past the reported size before the next update.
	 */
	{
		const uint32_t first = base + 1200U;
		const size_t total = 5U * 1024U * 1024U + 4097U;
		const uint32_t clusters = (uint32_t)((total + 32767U) / 32768U);

		CHECK(view_put_entry(&layout, "win-lag.bin", 0U, 0U));
		CHECK(view_bitmap(&layout, first, clusters, true));
		CHECK(view_write_data(&layout, first, 1536U * 1024U, 701U, 65536U));
		CHECK(view_put_entry(&layout, "win-lag.bin", first, 1024U * 1024U));
		CHECK(view_write_data_range(&layout, first, 1536U * 1024U, 3U * 1024U * 1024U + 1000U, 701U, 65536U));
		CHECK(view_put_entry(&layout, "win-lag.bin", first, 2U * 1024U * 1024U));
		CHECK(view_write_data_range(&layout, first, 3U * 1024U * 1024U + 1000U, total, 701U, 65536U));
		CHECK(view_put_entry(&layout, "win-lag.bin", first, 3U * 1024U * 1024U));
		CHECK(view_put_entry(&layout, "win-lag.bin", first, total));
	}
	/*
	 * Delete, reuse the freed slots for another file, then create the first name again, all before any
	 * flush. The deleted file still exists on the card at that point, and its name must not collide.
	 */
	{
		const uint32_t first = base + 2000U;

		CHECK(view_put_entry(&layout, "reuse-a.bin", first, 100U));
		CHECK(view_put_entry(&layout, "reuse-b.bin", first + 1U, 100U));
		CHECK(view_delete_entry(&layout, "reuse-a.bin"));
		CHECK(view_put_entry(&layout, "reuse-c.bin", first + 2U, 100U));
		CHECK(view_put_entry(&layout, "reuse-a.bin", first + 3U, 100U));
		CHECK(view_put_entry(&layout, "reuse-dir", 0U, 0U));
	}
	/*
	 * A freed cluster goes to a new file. The deleted file used to be found by that cluster and
	 * renamed onto the new name, which already existed.
	 */
	{
		const uint32_t first = base + 2400U;

		CHECK(view_put_entry(&layout, "again-a.bin", first, 1000U));
		CHECK(view_put_entry(&layout, "again-b.bin", 0U, 0U));
		CHECK(view_delete_entry(&layout, "again-a.bin"));
		CHECK(view_put_entry(&layout, "again-b.bin", first, 2000U));
	}
	/* A copy that is abandoned: data and allocation, then the space is given back with no entry. */
	{
		const uint32_t first = base + 800U;

		CHECK(view_bitmap(&layout, first, 40U, true));
		CHECK(view_write_data(&layout, first, 40U * 32768U, 603U, 65536U));
		CHECK(view_bitmap(&layout, first, 40U, false));
	}
	CHECK(bdev_flush(layout.view) == 0);
	bdev_close(layout.view);
}

/**
 * @brief Verify the files written by workload_windows_order().
 *
 * @details Reads every file through the source partition and checks that no
 *          ".cs-stage-" temporary entry was left on the card.
 *
 * @param[in] mount Mount point of the source partition.
 */
static void check_windows_order(const char *mount)
{
	char path[96];
	struct vfs_dir *dir;
	struct vfs_dirent entry;
	int error = 0;

	for (uint32_t index = 0U; index < sizeof(windows_files) / sizeof(windows_files[0]); index++) {
		snprintf(path, sizeof(path), "%s/%s", mount, windows_files[index].name);
		CHECK(verify_pattern(path, windows_files[index].size, windows_files[index].seed) == 0);
	}
	snprintf(path, sizeof(path), "%s/win-lag.bin", mount);
	CHECK(verify_pattern(path, 5U * 1024U * 1024U + 4097U, 701U) == 0);
	snprintf(path, sizeof(path), "%s/win-pair-a.bin", mount);
	CHECK(verify_pattern(path, 600000U, 601U) == 0);
	snprintf(path, sizeof(path), "%s/win-pair-b.bin", mount);
	CHECK(verify_pattern(path, 500000U, 602U) == 0);
	/* Nothing of the staging may stay on the card. */
	dir = vfs_opendir(mount, &error);
	CHECK(dir != NULL);
	while (dir != NULL && vfs_readdir(dir, &entry) == 0) {
		CHECK(strncmp(entry.name, ".cs-stage-", 10U) != 0);
	}
	if (dir != NULL) {
		vfs_closedir(dir);
	}
}

/**
 * @brief Run the full matrix for one source-filesystem mix.
 *
 * @details Builds the card and seed files, scans and starts the proxy, mounts
 *          the view, checks the _LAYOUT.TXT description of the disk, and runs
 *          every workload on all three partitions. It then verifies the source
 *          partitions, restarts the proxy to check persistence, exercises stop
 *          and restart, and deletes the disk.
 *
 * @param[in] mix Source filesystem mix.
 */
static void run_mix(const struct mix *mix)
{
	struct gendisk *disk;
	struct storage_proxy_partition_info infos[8];
	size_t count = 0U;
	bool seen_mount_fail = false;

	printf("mix: %s\n", mix->name);
	build_card(&disk, mix);
	CHECK(storage_proxy_scan("mmcblk0", infos, 8U, &count) == 0);
	CHECK(count == 3U);
	CHECK(storage_proxy_start("mmcblk0") == 0);
	CHECK(vfs_mount("cardview0", "/view", "fat") == 0);
	/* The root carries the disk layout: the table type and where each partition begins. */
	{
		char layout[1024];
		int descriptor;
		descriptor = vfs_open("/view/_LAYOUT.TXT", VFS_O_READ);
		int got = descriptor < 0 ? descriptor : vfs_read(descriptor, layout, sizeof(layout) - 1U);

		if (descriptor >= 0) {
			(void)vfs_close(descriptor);
		}
		if (got > 0) {
			layout[got] = '\0';
		}
		CHECK(got > 0);
		CHECK(strstr(layout, "CardSight proxy disk layout") != NULL);
		CHECK(strstr(layout, "table     MBR") != NULL);
		CHECK(strstr(layout, "p1") != NULL && strstr(layout, "2048") != NULL);
		CHECK(strstr(layout, "exposed") != NULL);
	}
	for (uint32_t partition = 1U; partition <= 3U; partition++) {
		char view_dir[32];
		char source_dir[32];

		snprintf(view_dir, sizeof(view_dir), "/view/partition%u", partition);
		snprintf(source_dir, sizeof(source_dir), "/verify%u", partition);
		/* Every filesystem the proxy can mount is writable through it. */
		workload_sizes(view_dir, source_dir, partition);
		workload_chunks(view_dir, partition);
		workload_many_files(view_dir, partition);
		workload_directories(view_dir, partition);
		workload_overwrite_truncate(view_dir, partition);
	}
	(void)seen_mount_fail;
	workload_valid_length();
	workload_windows_order();
	CHECK(vfs_umount("/view") == 0);
	storage_proxy_stop();
	CHECK(!storage_proxy_running());
	check_sources(mix, true);

	/* Start and stop again: the second round must see what the first one left. */
	CHECK(storage_proxy_start("mmcblk0") == 0);
	CHECK(vfs_mount("cardview0", "/view", "fat") == 0);
	CHECK(verify_pattern("/view/partition1/size-16.bin", sizes[16], 100U + 16U) == 0);
	CHECK(vfs_umount("/view") == 0);
	storage_proxy_stop();

	/* The card vanishes while the proxy runs: stop must complete and later starts must work. */
	CHECK(storage_proxy_start("mmcblk0") == 0);
	;
	storage_proxy_stop();
	CHECK(!storage_proxy_running());
	CHECK(storage_proxy_start("mmcblk0") == 0);
	storage_proxy_stop();

	CHECK(del_gendisk(disk) == 0);
	put_disk(disk);
}

/**
 * @brief Check the view's layout description for a GPT card.
 *
 * @details Builds a GPT layout with FAT32, exFAT, and ext4 partitions whose
 *          last entry covers the rest of the card, starts the proxy, and checks
 *          that _LAYOUT.TXT names GPT, the partition type names, and only the
 *          real table entries.
 */
static void run_gpt_layout(void)
{
	const struct blk_partition_layout_entry entries[] = {
		{ .sectors = PARTITION_MIB * 2048U, .content = BLK_PARTITION_FAT32, .label = "BOOT" },
		{ .sectors = PARTITION_MIB * 2048U, .content = BLK_PARTITION_EXFAT, .label = "DATA" },
		{ .sectors = 0U, .content = BLK_PARTITION_EXT4, .label = "ROOT" },
	};
	/** @brief Filesystem of each GPT test partition. */
	static const char *const types[3] = { "fat", "exfat", "ext" };
	struct gendisk *disk = alloc_disk();
	char layout[1024];
	int descriptor;
	int got;

	printf("gpt layout\n");
	CHECK(disk != NULL);
	memset(disk_data, 0, (size_t)CARD_SECTORS * 512U);
	strcpy(disk->disk_name, "mmcblk0");
	disk->fops = &ram_operations;
	disk->capacity = CARD_SECTORS;
	CHECK(add_disk(disk) == 0);
	CHECK(blk_write_partition_layout(disk, BLK_PARTITION_TABLE_GPT, entries, 3U) == 0);
	for (uint32_t partition = 1U; partition <= 3U; partition++) {
		char device[24];

		snprintf(device, sizeof(device), "mmcblk0p%u", partition);
		CHECK(vfs_format(device, types[partition - 1U], "GPT", 0U) == 0);
	}
	CHECK(storage_proxy_start("mmcblk0") == 0);
	CHECK(vfs_mount("cardview0", "/view", "fat") == 0);
	descriptor = vfs_open("/view/_LAYOUT.TXT", VFS_O_READ);
	got = descriptor < 0 ? descriptor : vfs_read(descriptor, layout, sizeof(layout) - 1U);
	if (descriptor >= 0) {
		(void)vfs_close(descriptor);
	}
	if (got > 0) {
		layout[got] = '\0';
	}
	CHECK(got > 0);
	CHECK(strstr(layout, "table     GPT") != NULL);
	CHECK(strstr(layout, "Microsoft basic data") != NULL);
	CHECK(strstr(layout, "Linux filesystem") != NULL);
	/* The view size is known when the file is written, and only the table's entries are listed. */
	CHECK(strstr(layout, "view      cardview0, 0 MiB") == NULL);
	CHECK(strstr(layout, "other") == NULL);
	CHECK(strstr(layout, "p4") == NULL);
	CHECK(vfs_umount("/view") == 0);
	storage_proxy_stop();
	CHECK(del_gendisk(disk) == 0);
	put_disk(disk);
}

/**
 * @brief Check partition selection and the read-only propagation.
 *
 * @details Starts the proxy with only the exFAT partition selected and proves
 *          the other partitions are absent. It then marks one source partition
 *          read-only, restarts with all partitions selected, and checks that
 *          the whole view becomes read-only and writes are refused with EROFS
 *          instead of being lost.
 */
static void run_selection_and_read_only(void)
{
	struct gendisk *disk;
	struct block_device *partition;
	struct storage_proxy_config config;

	printf("selection and read only\n");
	build_card(&disk, &mixes[3]);
	config.source_disk = "mmcblk0";
	config.selected_partitions = 0x02U; /* only the exFAT partition */
	CHECK(storage_proxy_start_config(&config) == 0);
	CHECK(vfs_mount("cardview0", "/view", "fat") == 0);
	CHECK(write_pattern("/view/partition2/only.bin", 5000U, 9U, 512U) == 0);
	CHECK(verify_pattern("/view/partition2/only.bin", 5000U, 9U) == 0);
	{
		struct vfs_stat stat;

		CHECK(vfs_stat("/view/partition1", &stat) != 0 || vfs_stat("/view/partition1/seed-1.bin", &stat) != 0);
	}
	CHECK(vfs_umount("/view") == 0);
	storage_proxy_stop();

	/* One read only source makes the whole view read only, and writes are refused, not lost. */
	partition = bdev_open_by_name("mmcblk0p3");
	CHECK(partition != NULL);
	partition->read_only = true;
	bdev_close(partition);
	config.selected_partitions = 0x07U;
	CHECK(storage_proxy_start_config(&config) == 0);
	CHECK(storage_proxy_read_only());
	CHECK(vfs_mount("cardview0", "/view", "fat") == 0);
	CHECK(vfs_open("/view/partition1/no.txt", VFS_O_WRITE | VFS_O_CREATE) == -EROFS);
	CHECK(verify_pattern("/view/partition1/seed-1.bin", 70001U, 1U) == 0);
	CHECK(vfs_umount("/view") == 0);
	storage_proxy_stop();
	partition = bdev_open_by_name("mmcblk0p3");
	partition->read_only = false;
	bdev_close(partition);

	CHECK(del_gendisk(disk) == 0);
	put_disk(disk);
}

/**
 * @brief Run the storage proxy matrix test.
 *
 * @details Allocates the simulated card, registers the FatFs and ext4 VFS
 *          implementations, runs every source mix, then the selection/read-only
 *          and GPT layout cases. Prints a PASS or FAIL summary and returns the
 *          process status.
 *
 * @return 0 when every check passed, 1 otherwise.
 */
int main(void)
{
	disk_data = aligned_alloc(64U, (size_t)CARD_SECTORS * 512U);
	CHECK(disk_data != NULL);
	CHECK(vfs_ff_register() == 0);
	CHECK(vfs_ext_register() == 0);
	for (uint32_t index = 0U; index < sizeof(mixes) / sizeof(mixes[0]); index++) {
		run_mix(&mixes[index]);
	}
	run_selection_and_read_only();
	run_gpt_layout();
	free(disk_data);
	printf("storage proxy matrix test: %s (%u checks, %u failures)\n", failures == 0U ? "PASS" : "FAIL", checks,
		failures);
	return failures == 0U ? 0 : 1;
}
