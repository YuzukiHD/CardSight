// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_shell.c
 * @brief The lazyproxy shell command.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

#ifdef CONFIG_STORAGE_PROXY_SHELL
/**
 * @brief Build a partition mask from the partitions that exist on a disk.
 *
 * @param[in] source_disk Whole-disk block device name.
 * @return The mask of present partitions, possibly zero.
 */
uint32_t lazy_default_mask(const char *source_disk)
{
	uint32_t mask = 0U;

	for (uint32_t partition = 0U; source_disk != NULL &&
	     partition < PROXY_LAZY_PARTITIONS; partition++) {
		char device[VFS_BLK_NAME];
		struct block_device *bdev;

		snprintf(device, sizeof(device), "%sp%u", source_disk, partition + 1U);
		bdev = bdev_open_by_name(device);
		if (bdev != NULL) {
			mask |= 1U << partition;
			bdev_close(bdev);
		}
	}
	return mask;
}

/**
 * @brief Entry point of the lazyproxy shell command.
 *
 * @details Subcommands: "start <disk> [mask]" starts the view, "stop" stops
 * it, "usb on|rw|off" exports it as a read-only or read-write USB mass storage
 * device, and "status" prints the geometry, reservations, directory and cache
 * usage and the last error. With no argument it behaves like "status".
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or a negative errno.
 */
int command_lazyproxy(int argc, char **argv)
{
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "start") == 0) {
		uint32_t mask = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) :
			lazy_default_mask(argv[2]);
		int result = storage_proxy_lazy_start(argv[2], mask);

		printf(result == 0 ? "lazy proxy started\n" : "lazy proxy start failed: %d\n", result);
		return result;
	}
	if (argc == 2 && strcmp(argv[1], "stop") == 0) {
		storage_proxy_lazy_stop();
		printf("lazy proxy stopped\n");
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "usb") == 0) {
#ifndef STORAGE_PROXY_HOST_TEST
		if (strcmp(argv[2], "off") == 0) {
			int result = usb_mass_storage_stop();

			lazy.usb_active = false;
			printf("lazy proxy USB stopped: %d\n", result);
			return result;
		}
		if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "rw") == 0) {
			bool read_only = strcmp(argv[2], "on") == 0;
			int result;

			if (!lazy.running) {
				return -ENODEV;
			}
			if (usb_mass_storage_running()) {
				return -EBUSY;
			}
			usb_mass_storage_force_read_only(read_only);
			result = usb_mass_storage_start_device(PROXY_LAZY_DISK_NAME);
			lazy.usb_active = result == 0;
			printf("lazy proxy USB %s: %d\n", result == 0 ? "started" : "failed", result);
			return result;
		}
#endif
		return -EINVAL;
	}
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
		struct storage_proxy_lazy_layout layout;
		uint32_t active_runs = 0U;

		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
			if (lazy.runs != NULL && lazy.runs[index].used) {
				active_runs++;
			}
		}
		printf("lazy proxy: %s, source %s, dirs %u, ranges %u\n",
			lazy.running ? "running" : "stopped", lazy.source_name,
			lazy.dir_count, lazy.range_count);
		printf("  cache %u entries, %u bytes, %u evictions, truncated %d, last error %d\n",
			lazy.cache_entries, lazy.cache_bytes, lazy.evictions, lazy.truncated ? 1 : 0,
			lazy.last_error);
		printf("  overlay %u records, pending %u sectors, staged %llu sectors in %u/%u runs, clean %d, flush failed %d\n",
			lazy.overlay_count, lazy.pending_count,
			(unsigned long long)lazy.staged_sectors, active_runs,
			(unsigned int)CONFIG_STORAGE_PROXY_LAZY_RUNS, lazy.clean ? 1 : 0,
			lazy.flush_failed ? 1 : 0);
		printf("  read %llu sectors, served %llu file bytes\n",
			(unsigned long long)lazy.sectors_read, (unsigned long long)lazy.file_bytes);
		printf("  wrote %llu sectors, stored %llu file bytes\n",
			(unsigned long long)lazy.sectors_written,
			(unsigned long long)lazy.file_bytes_written);
		if (storage_proxy_lazy_layout(&layout) == 0) {
			printf("  view %u clusters, data at sector %u, bitmap cluster %u (%u bytes), "
				"root cluster %u\n", layout.total_clusters, layout.data_start,
				layout.bitmap_cluster, layout.bitmap_bytes, layout.root_cluster);
			for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
				if (!layout.selected[partition]) {
					continue;
				}
				printf("  p%u: reserve %u+%u, folder %u+%u\n", partition + 1U,
					layout.reserve_first[partition], layout.reserve_clusters[partition],
					layout.partition_root_first[partition],
					layout.partition_root_clusters[partition]);
			}
			printf("  tail: %u+%u clusters free\n", layout.tail_first, layout.tail_clusters);
		}
		return 0;
	}
	printf("lazyproxy [start <disk> [mask]|stop|usb on|rw|off|status]\n");
	return -EINVAL;
}
/** @brief Register @c lazyproxy as a shell command with its usage text. */
SHELL_CMD(lazyproxy, "lazyproxy [start <disk> [mask]|stop|usb on|rw|off|status]: lazy exFAT proxy", command_lazyproxy);
#endif
