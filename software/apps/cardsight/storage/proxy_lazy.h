/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file proxy_lazy.h
 * @brief Public interface of the lazy exFAT proxy view.
 *
 * The lazy engine synthesises a second exFAT volume, exposed as block device
 * @c cardview1, whose partition folders are backed by the mounted source file
 * systems. Unlike the eager engine it indexes a source directory only when the
 * host first reads it, keeps those indexes in a bounded LRU cache, and never
 * changes the allocation bitmap after the volume is registered. The view is
 * read-write: host changes go back to the source volumes through a bounded
 * per-view overlay and pending-data table.
 */

#ifndef CARDSIGHT_STORAGE_PROXY_LAZY_H
#define CARDSIGHT_STORAGE_PROXY_LAZY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage_proxy.h"

/**
 * @brief Geometry and reservation regions of a running lazy view.
 */
struct storage_proxy_lazy_layout {
	/** @brief Number of clusters in the view. */
	uint32_t total_clusters;
	/** @brief First sector of cluster two. */
	uint32_t data_start;
	/** @brief First cluster of the allocation bitmap. */
	uint32_t bitmap_cluster;
	/** @brief Useful bytes of the allocation bitmap. */
	uint32_t bitmap_bytes;
	/** @brief First cluster of the synthesized root directory. */
	uint32_t root_cluster;
	/** @brief Number of clusters the root directory spans. */
	uint32_t root_clusters;
	/** @brief First free cluster after every reservation. */
	uint32_t tail_first;
	/** @brief Number of free clusters the host sees as the tail. */
	uint32_t tail_clusters;
	/** @brief First cluster reserved for each selected partition. */
	uint32_t reserve_first[STORAGE_PROXY_MAX_PARTITIONS];
	/** @brief Number of clusters reserved for each selected partition. */
	uint32_t reserve_clusters[STORAGE_PROXY_MAX_PARTITIONS];
	/** @brief First cluster of a partition's view folder, or zero before it is indexed. */
	uint32_t partition_root_first[STORAGE_PROXY_MAX_PARTITIONS];
	/** @brief Number of clusters a partition's view folder spans, or zero before it is indexed. */
	uint32_t partition_root_clusters[STORAGE_PROXY_MAX_PARTITIONS];
	/** @brief True when the partition was selected for the view. */
	bool selected[STORAGE_PROXY_MAX_PARTITIONS];
};

/**
 * @brief Start the lazy view over selected partitions of a disk.
 *
 * @details Probes and mounts the selected partitions, sizes one reservation
 * region per partition, builds the boot region, FAT, allocation bitmap,
 * up-case table and root directory, marks every reservation used, and
 * registers @c cardview1. No source directory is indexed here.
 *
 * @param[in] source_disk          Whole-disk block device name.
 * @param[in] selected_partitions Bit mask of partitions; bit zero is partition one.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_lazy_start(const char *source_disk, uint32_t selected_partitions);

/**
 * @brief Stop the lazy view and release everything it holds.
 */
void storage_proxy_lazy_stop(void);

/**
 * @brief Report whether the lazy view is registered as a block device.
 *
 * @return True while @c cardview1 exists.
 */
bool storage_proxy_lazy_running(void);

/**
 * @brief Return the block device name the lazy view is registered under.
 *
 * @return The constant device name of the lazy view.
 */
const char *storage_proxy_lazy_device_name(void);

/**
 * @brief Copy the geometry and reservation regions of the running view.
 *
 * @param[out] layout Receives the layout.
 * @return 0 on success, -ENODEV when the view is stopped, or -ETIMEDOUT when
 *         the view lock stayed busy.
 */
int storage_proxy_lazy_layout(struct storage_proxy_lazy_layout *layout);

/**
 * @brief Return the number of source directories the view has indexed.
 *
 * @return Directory header count, including the root and the partition folders.
 */
size_t storage_proxy_lazy_directory_count(void);

/**
 * @brief Return the number of directory entries currently cached.
 *
 * @return Cached entry count.
 */
uint32_t storage_proxy_lazy_cached_entries(void);

/**
 * @brief Return the number of bytes the directory entry caches currently hold.
 *
 * @return Cached byte count, including names and cluster order arrays.
 */
uint32_t storage_proxy_lazy_cached_bytes(void);

/**
 * @brief Drop every cached directory entry, keeping extents and the overlay.
 *
 * @details A test or diagnostic helper: the next read reloads each directory
 * from the source readdir result merged with the overlay, which is what proves
 * host-created entries survive cache eviction.
 */
void storage_proxy_lazy_cache_drop(void);

#endif /* CARDSIGHT_STORAGE_PROXY_LAZY_H */
