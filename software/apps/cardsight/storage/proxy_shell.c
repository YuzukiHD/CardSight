// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_shell.c
 * @brief The storageproxy shell command and the counters it prints.
 */

#include "proxy_internal.h"

#ifdef CONFIG_STORAGE_PROXY_SHELL
#ifndef STORAGE_PROXY_HOST_TEST
#include "task.h"
/** @brief Yield the shell task during a long count without blocking the UI. */
#define proxy_count_yield() taskYIELD()
#else
/** @brief The host test has no scheduler; the count runs straight through. */
#define proxy_count_yield() ((void)0)
#endif

/**
 * @brief Counters gathered by one recursive source-tree walk.
 */
struct proxy_count {
	/** @brief Entries seen, including directories. */
	uint64_t entries;
	/** @brief Regular files seen. */
	uint64_t files;
	/** @brief Directories seen. */
	uint64_t directories;
	/** @brief Sum of the file sizes in bytes. */
	uint64_t bytes;
	/** @brief Sum of the name lengths in bytes. */
	uint64_t name_bytes;
	/** @brief Deepest directory level reached. */
	uint32_t max_depth;
	/** @brief Directories that could not be opened. */
	uint32_t errors;
	/** @brief Entries whose full path did not fit in VFS_PATH_MAX. */
	uint32_t too_long;
};

/**
 * @brief Walk one source directory and add its entries to the counters.
 *
 * @param[in] path  Absolute path of the directory.
 * @param[in] depth Current depth below the partition root.
 * @param[in,out] count Counters to update.
 *
 * The walk opens one directory at a time and yields every 256 entries, so the
 * UI task keeps running while the shell performs the measurement.
 */
static void proxy_count_path(char *path, size_t length, uint32_t depth, uint32_t limit,
	struct proxy_count *count)
{
	struct vfs_dir *dir;
	struct vfs_dirent entry;
	int error = 0;

	dir = vfs_opendir(path, &error);
	if (dir == NULL) {
		count->errors++;
		return;
	}
	while (vfs_readdir(dir, &entry) == 0) {
		count->entries++;
		count->name_bytes += strlen(entry.name);
		if (entry.st.dir) {
			size_t name_length = strlen(entry.name);

			count->directories++;
			if (depth + 1U > count->max_depth) {
				count->max_depth = depth + 1U;
			}
			/* Extend the one path buffer in place instead of copying it per level. */
			if (length + 1U + name_length >= VFS_PATH_MAX) {
				count->too_long++;
			} else if (depth + 1U < limit) {
				path[length] = '/';
				memcpy(path + length + 1U, entry.name, name_length + 1U);
				proxy_count_path(path, length + 1U + name_length, depth + 1U, limit, count);
				path[length] = '\0';
			}
		} else {
			count->files++;
			count->bytes += entry.st.size;
		}
		if ((count->entries & 0xffU) == 0U) {
			proxy_count_yield();
		}
		if ((count->entries & 0xfffU) == 0U) {
			printf("  ... %llu entries, %llu dirs at %s\n",
				(unsigned long long)count->entries,
				(unsigned long long)count->directories, path);
		}
	}
	vfs_closedir(dir);
}

/**
 * @brief Mount each selected partition read-only and count its source tree.
 *
 * @param[in] disk Source disk name, such as "mmcblk0".
 * @param[in] mask Partition mask, or zero to count every present partition.
 * @return 0 on success, or a negative errno from the scan or a mount.
 *
 * The command builds no proxy index and allocates no nodes; it is the
 * measurement used to size the lazy directory cache on a real card.
 */
static int command_storageproxy_count(const char *disk, uint32_t mask, uint32_t limit)
{
	struct storage_proxy_partition_info partitions[STORAGE_PROXY_MAX_PARTITIONS];
	size_t count = 0U;
	int result;

	result = storage_proxy_scan(disk, partitions, sizeof(partitions) / sizeof(partitions[0]), &count);
	if (result != 0) {
		printf("storage proxy count: scan failed: %d\n", result);
		return result;
	}
	printf("storage proxy count: %u partition(s) on %s\n", (unsigned)count, disk);
	for (size_t index = 0U; index < count; index++) {
		struct storage_proxy_partition_info *part = &partitions[index];
		struct proxy_count stats;
		char path[VFS_PATH_MAX];
		char target[16];
		uint32_t bit = 1U << (part->partno - 1U);
		int written;

		if (!part->present || (mask != 0U && (mask & bit) == 0U)) {
			continue;
		}
		if (part->status != 0) {
			printf("  %s: probe failed: %d\n", part->device, part->status);
			continue;
		}
		snprintf(target, sizeof(target), "/count%u", (unsigned)part->partno);
		result = vfs_mount_ex(part->device, target, NULL, VFS_MOUNT_READ_ONLY);
		if (result != 0) {
			printf("  %s: mount failed: %d\n", part->device, result);
			continue;
		}
		written = snprintf(path, sizeof(path), "%s", target);
		if (written < 0 || (size_t)written >= sizeof(path)) {
			(void)vfs_umount(target);
			continue;
		}
		memset(&stats, 0, sizeof(stats));
		proxy_count_path(path, (size_t)written, 0U, limit, &stats);
		(void)vfs_umount(target);
		printf("  %s: %llu entries, %llu files, %llu dirs, %llu bytes, %llu name bytes, "
			"depth %u (limit %u), %u errors, %u too long\n", part->device,
			(unsigned long long)stats.entries, (unsigned long long)stats.files,
			(unsigned long long)stats.directories, (unsigned long long)stats.bytes,
			(unsigned long long)stats.name_bytes, (unsigned)stats.max_depth,
			(unsigned)limit, (unsigned)stats.errors, (unsigned)stats.too_long);
	}
	return 0;
}

/**
 * @brief Entry point of the storageproxy shell command.
 * @brief Entry point of the storageproxy shell command.
 *
 * @details Subcommands: "start <disk> [mask]" starts the view (all present
 * partitions when no mask is given), "stop" stops it, "status" prints the node
 * and string usage, the per-partition probe results and the write-path
 * timings, and "reset" clears the counters. With no argument it behaves like
 * "status".
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or a negative errno.
 */
static int command_storageproxy(int argc, char **argv)
{
	if ((argc >= 3 && argc <= 5) && strcmp(argv[1], "count") == 0) {
		uint32_t mask = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0U;
		uint32_t limit = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 0) : UINT32_MAX;

		if (limit == 0U) {
			limit = UINT32_MAX;
		}
		return command_storageproxy_count(argv[2], mask, limit);
	}
	if (argc == 2 && strcmp(argv[1], "stop") == 0) {
		storage_proxy_stop();
		printf("storage proxy stopped\n");
		return 0;
	}
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "start") == 0) {
		const struct storage_proxy_config config = {
			.source_disk = argv[2],
			.selected_partitions = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0U,
		};
		int result = argc == 4 ? storage_proxy_start_config(&config) : storage_proxy_start(argv[2]);

		printf(result == 0 ? "storage proxy started\n" : "storage proxy start failed: %d\n", result);
		return result;
	}
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
		printf("storage proxy: %s, %u entries", proxy.running ? "running" : "stopped", proxy.node_count);
		for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
			printf(", p%u=%d", partition + 1U, proxy.partitions[partition].status);
		}
		printf("\n");
		printf("  nodes %u of %u, strings %u of %u bytes, pending peak %u sectors\n", proxy.node_count,
			(uint32_t)CONFIG_STORAGE_PROXY_MAX_ENTRIES, proxy.string_used,
			(uint32_t)CONFIG_STORAGE_PROXY_STRING_BYTES, (uint32_t)proxy_stats.pending_peak);
		printf("  limits: index full %u, strings full %u, directory capacity %u\n",
			(uint32_t)proxy_stats.index_full, (uint32_t)proxy_stats.strings_full,
			(uint32_t)proxy_stats.dir_capacity_exceeded);
		printf("  written %llu sectors, %llu bytes to files, last sync error %d\n",
			(unsigned long long)proxy_stats.sectors, (unsigned long long)proxy_stats.file_bytes, proxy_stats.last_sync_error);
		printf("  staged %llu sectors, runs renamed %u, copied %u\n", (unsigned long long)proxy_stats.staged_sectors,
			(uint32_t)proxy_stats.runs_renamed, (uint32_t)proxy_stats.runs_copied);
		{
			/** @brief Labels of the timing counters, in the order of @c timings. */
			static const char *const names[] = { "disk write", "batch flush", "directory", "pending store",
				"pending flush", "entry sync" };
			/** @brief Timing counters printed by the status command. */
			const struct proxy_timing *timings[] = { &proxy_stats.disk_write, &proxy_stats.batch_flush,
				&proxy_stats.directory_write, &proxy_stats.pending_store, &proxy_stats.pending_flush,
				&proxy_stats.sync_entry };

			for (uint32_t index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
				printf("  %-14s %8llu calls %10llu us (%llu us/call)\n", names[index],
					(unsigned long long)timings[index]->calls, (unsigned long long)timings[index]->microseconds,
					timings[index]->calls != 0U ? (unsigned long long)(timings[index]->microseconds / timings[index]->calls) : 0ULL);
			}
		}
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "reset") == 0) {
		memset(&proxy_stats, 0, sizeof(proxy_stats));
		return 0;
	}
	printf("storageproxy [start <disk> [partition mask]|stop|status|count <disk> [partition mask] [depth]|reset]\n");
	return -EINVAL;
}

/** @brief Register @c storageproxy as a shell command with its usage text. */
SHELL_CMD(storageproxy, "storageproxy [start <disk> [partition mask]|stop|status|count <disk> [partition mask] [depth]|reset]: partition proxy", command_storageproxy);
#endif
