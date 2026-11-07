// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_shell.c
 * @brief The storageproxy shell command and the counters it prints.
 */

#include "proxy_internal.h"

#ifdef CONFIG_STORAGE_PROXY_SHELL
/**
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
	printf("storageproxy [start <disk> [partition mask]|stop|status|reset]\n");
	return -EINVAL;
}

/** @brief Register @c storageproxy as a shell command with its usage text. */
SHELL_CMD(storageproxy, "storageproxy [start <disk> [partition mask]|stop|status|reset]: partition proxy", command_storageproxy);
#endif
