// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_types.h
 * @brief Shared state, constants and inline helpers of the eager exFAT proxy.
 *
 * @details The whole view is described by one @c storage_proxy instance that is
 * protected by its own mutex. This header holds the view state and the small
 * geometry/byte-order helpers every module uses.
 */
#ifndef CARDSIGHT_STORAGE_EAGER_PROXY_TYPES_H
#define CARDSIGHT_STORAGE_EAGER_PROXY_TYPES_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <init.h>
#ifdef STORAGE_PROXY_HOST_TEST
#include <log.h>
/**
 * @brief Host-test stand-in for the kernel microsecond clock.
 *
 * @return Always zero; timing statistics are meaningless under the host test.
 */
static inline uint64_t ktime_get_us(void)
{
	return 0U;
}
/**
 * @brief Host-test stand-in for the kernel mutex.
 */
struct mutex {
	/** @brief Only checked for being non-NULL by the initialisation code. */
	void *handle;
};
/** @brief Host-test stand-in for the kernel's infinite wait. */
#define WAIT_FOREVER (-1)
/**
 * @brief Host-test stand-in that marks the mutex as initialised.
 *
 * @param[in,out] lock Mutex to initialise.
 */
static inline void mutex_init(struct mutex *lock)
{
	lock->handle = lock;
}
/**
 * @brief Host-test stand-in that takes no lock.
 *
 * @param[in] lock    Ignored.
 * @param[in] timeout Ignored.
 * @return Always zero.
 */
static inline int mutex_lock_timeout(struct mutex *lock, int timeout)
{
	(void)lock;
	(void)timeout;
	return 0;
}
/**
 * @brief Host-test stand-in that releases no lock.
 *
 * @param[in] lock Ignored.
 * @return Always zero.
 */
static inline int mutex_unlock(struct mutex *lock)
{
	(void)lock;
	return 0;
}
#else
#include "FreeRTOS.h"
#include <hal/kernel.h>
#include <hal/ktime.h>
#endif
#include <hal/blkdev.h>
#include "storage/storage_proxy.h"
#include <shell.h>
#include <vfs.h>

#include "ff.h"

#ifndef CONFIG_STORAGE_PROXY_MAX_ENTRIES
/** @brief Default number of index nodes when the configuration does not set one. */
#define CONFIG_STORAGE_PROXY_MAX_ENTRIES 2048
#endif
#ifndef CONFIG_STORAGE_PROXY_STRING_BYTES
/** @brief Default size of the string arena when the configuration does not set one. */
#define CONFIG_STORAGE_PROXY_STRING_BYTES 262144
#endif
/** @brief Number of source partitions the view can expose; alias of the public limit. */
#define PROXY_PARTITIONS STORAGE_PROXY_MAX_PARTITIONS
/** @brief Logical and physical sector size of the view, in bytes. */
#define PROXY_SECTOR_SIZE 512U
/** @brief Sectors per cluster of the synthesized volume. */
#define PROXY_SECTORS_PER_CLUSTER 64U
/** @brief Cluster size of the view in bytes (32 KiB). */
#define PROXY_CLUSTER_SIZE (PROXY_SECTOR_SIZE * PROXY_SECTORS_PER_CLUSTER)
/** @brief Clusters reserved for a synthetic directory that is not backed by the source. */
#define PROXY_DIRECTORY_CLUSTERS 1U
/** @brief Number of boot region sectors before the FAT (12 records plus checksum). */
#define PROXY_BOOT_SECTORS 24U
/** @brief Sector where the FAT starts. */
#define PROXY_FAT_OFFSET 32U
/** @brief First real cluster number of an exFAT volume. */
#define PROXY_ROOT_CLUSTER_MIN 2U
/** @brief FAT entry value that ends a chain. */
#define PROXY_EOC 0xffffffffU
/** @brief Block device name the view is registered under. */
#define PROXY_DISK_NAME "cardview0"
/** @brief Node index value meaning "no node". */
#define PROXY_INVALID_NODE UINT32_MAX
/** @brief Size of the staging/batch buffer, in bytes. */
#define PROXY_WRITE_BUFFER (128U * 1024U)
/* Data that arrives before its directory entry waits here; the table is 520 B per sector. */
#ifndef PROXY_PENDING_MAX
/** @brief Maximum number of sectors kept in RAM while their directory entry is missing (512 KiB). */
#define PROXY_PENDING_MAX 1024U
#endif
/* Data of a file whose directory entry has not arrived is staged on the source card past this much RAM. */
/** @brief Number of staging runs that can be open at once. */
#define PROXY_RUNS 8U
#ifndef PROXY_LOCK_TIMEOUT_MS
/** @brief Longest a block device callback waits for the proxy lock, in milliseconds. */
#define PROXY_LOCK_TIMEOUT_MS 2000
#endif
/** @brief Clusters past the end of a run that still count as belonging to it. */
#define PROXY_RUN_GAP 64U
/** @brief Prefix of the staging files written next to the source files. */
#define PROXY_STAGE_PREFIX ".cs-stage-"

/**
 * @brief Where the data of an indexed node comes from.
 */
enum proxy_source {
	/** @brief No data; the object only exists in the view. */
	PROXY_SOURCE_NONE,
	/** @brief Backed by a file or directory in a mounted source volume. */
	PROXY_SOURCE_VFS,
	/** @brief Read-only text held in the string arena. */
	PROXY_SOURCE_MEMORY,
	/** @brief Data the host wrote to a directory entry with no source file. */
	PROXY_SOURCE_VOLATILE,
};

/**
 * @brief One object in the view: a file, a directory or a deleted entry.
 */
struct proxy_node {
	/** @brief Index of the parent node. */
	uint32_t parent;
	/** @brief First child, or PROXY_INVALID_NODE. */
	uint32_t first_child;
	/** @brief Last child, so appending does not walk the list. */
	uint32_t last_child;
	/** @brief Next sibling in the parent's list. */
	uint32_t next_sibling;
	/** @brief First 32-byte directory slot the object occupies. */
	uint32_t directory_slot;
	/** @brief Offset of the display name in the string arena. */
	uint32_t name_offset;
	/** @brief Offset of the source path (or message text) in the string arena. */
	uint32_t source_offset;
	/** @brief First cluster of the object, or zero when it has no extent. */
	uint32_t first_cluster;
	/** @brief Number of contiguous clusters the object owns. */
	uint32_t cluster_count;
	/** @brief File size in bytes. */
	uint64_t size;
	/** @brief Where the data comes from. */
	uint8_t source;
	/** @brief True for a directory. */
	bool directory;
	/** @brief True once the children of a source directory have been indexed. */
	bool scanned;
	/** @brief True when the host deleted the object. */
	bool deleted;
	/** @brief True when the host reused this entry's slots, so it is hidden from the directory. */
	bool retired;
	/** @brief Deleted placeholder that keeps the slots of a moved object a hole. */
	bool tombstone;
};

/**
 * @brief One sector of a file that arrived before its directory entry owned it.
 */
struct proxy_pending_sector {
	/** @brief Cluster the sector belongs to. */
	uint32_t cluster;
	/** @brief Sector offset inside the cluster. */
	uint16_t sector;
	/** @brief False once the sector was consumed, freed or superseded. */
	bool used;
	/** @brief Sector contents. */
	uint8_t data[PROXY_SECTOR_SIZE];
};

/**
 * @brief A source partition exposed as a folder of the view.
 */
struct proxy_partition {
	/** @brief Block device name of the partition. */
	char device[VFS_BLK_NAME];
	/** @brief Mount target used while the partition is a source. */
	char mount[16];
	/** @brief Volume information from the probe. */
	struct vfs_volume_info volume;
	/** @brief First sector of the partition on the source disk. */
	uint64_t start_sector;
	/** @brief Size of the partition in sectors. */
	uint64_t sectors;
	/** @brief Probe result, or -ENODEV when the partition is absent. */
	int status;
	/** @brief MBR partition type. */
	uint8_t type;
	/** @brief True when the partition device exists. */
	bool present;
	/** @brief True while the partition is mounted as a source. */
	bool mounted;
	/** @brief True when the partition was selected for the view. */
	bool selected;
	/** @brief True when the source volume accepts writes. */
	bool writable;
};

/**
 * @brief A contiguous stretch of clusters staged in a source file before its entry.
 */
struct proxy_run {
	/** @brief True while the run slot is in use. */
	bool used;
	/** @brief First cluster covered by the run. */
	uint32_t first_cluster;
	/** @brief Number of clusters covered so far. */
	uint32_t clusters;
	/* One bit per cluster whose data has been copied to its file in full; only runs up to 2048 clusters are tracked. */
	/** @brief One bit per cluster already copied to its owning file. */
	uint8_t claimed[256];
	/** @brief Partition index the staging file lives on. */
	uint32_t partition;
	/** @brief Path of the staging file. */
	char path[48];
};

/** @brief Longest run whose claimed bits are tracked; longer runs stay until the proxy stops. */
#define PROXY_RUN_TRACKED 2048U

/**
 * @brief The whole synthesized volume: index, geometry, caches and write-back state.
 */
struct storage_proxy {
	/** @brief Index nodes, @c node_count of them are live. */
	struct proxy_node *nodes;
	/** @brief Node indices sorted by first cluster for binary search. */
	uint32_t *range_nodes;
	/** @brief Arena holding names and source paths. */
	char *strings;
	/** @brief Up-case table, two bytes per UTF-16 code unit. */
	uint8_t *upcase;
	/** @brief Allocation bitmap of the view, one bit per cluster. */
	uint8_t *bitmap;
	/** @brief Number of index nodes allocated. */
	uint32_t node_count;
	/** @brief Number of valid entries in @c range_nodes. */
	uint32_t range_count;
	/** @brief Bytes of @c strings already handed out. */
	uint32_t string_used;
	/** @brief FAT sectors in the view. */
	uint32_t fat_sectors;
	/** @brief First data sector of the view. */
	uint32_t data_start;
	/** @brief Number of clusters in the view. */
	uint32_t total_clusters;
	/** @brief First cluster of the allocation bitmap. */
	uint32_t bitmap_cluster;
	/** @brief Number of clusters the allocation bitmap spans. */
	uint32_t bitmap_clusters;
	/** @brief Useful bytes of the allocation bitmap. */
	uint32_t bitmap_bytes;
	/** @brief First cluster of the up-case table. */
	uint32_t upcase_cluster;
	/** @brief Number of clusters the up-case table spans. */
	uint32_t upcase_clusters;
	/** @brief Useful bytes of the up-case table. */
	uint32_t upcase_bytes;
	/** @brief Checksum stored in the up-case table directory entry and the boot sector. */
	uint32_t upcase_checksum;
	/** @brief Cluster of the root directory. */
	uint32_t root_cluster;
	/** @brief First cluster never handed out yet. */
	uint32_t next_cluster;
	/** @brief Total sectors of the view. */
	uint64_t total_sectors;
	/** @brief Sectors of the source disk, which sizes the view. */
	uint64_t source_sectors;
	/** @brief Block device name of the source disk. */
	char source_name[VFS_BLK_NAME];
	/** @brief Volume flags from the boot sector (dirty and media failure bits). */
	uint16_t volume_flags;
	/** @brief Sectors waiting for a directory entry, in arrival order. */
	struct proxy_pending_sector *pending;
	/** @brief Hash index into @c pending keyed by (cluster, sector); entry plus one, zero is empty. */
	uint32_t *pending_index;
	/** @brief Number of entries used in @c pending. */
	uint32_t pending_count;
	/** @brief Allocated capacity of @c pending. */
	uint32_t pending_capacity;
	/** @brief Size of @c pending_index, always a power of two. */
	uint32_t pending_index_size;
	/** @brief Batches consecutive sectors of one file into a single write. */
	uint8_t write_buffer[PROXY_WRITE_BUFFER] __attribute__((aligned(64)));
	/** @brief Source path the batch buffer is destined for. */
	const char *write_path;
	/** @brief Descriptor of the staging file kept open, so appends need no open, seek or close. */
	int stage_fd;
	/** @brief Path of the staging file @c stage_fd refers to. */
	const char *stage_path;
	/** @brief Position of the next append in the staging file, or UINT64_MAX when unknown. */
	uint64_t stage_position;
	/** @brief Zero buffer used to fill gaps before an out-of-order write. */
	uint8_t fill_buffer[4096];
	/** @brief Staging runs opened for data without a directory entry. */
	struct proxy_run runs[PROXY_RUNS];
	/** @brief Offset of the batch buffer's first sector. */
	uint64_t write_offset;
	/** @brief Bytes used in the batch buffer. */
	uint32_t write_length;
	/** @brief Source partitions of the view. */
	struct proxy_partition partitions[PROXY_PARTITIONS];
	/** @brief Block device the view is registered as. */
	struct gendisk *disk;
	/** @brief Lock held by every block device callback and every start/stop. */
	struct mutex lock;
	/** @brief Cached read descriptor, or -1. */
	int cached_fd;
	/** @brief Node @c cached_fd belongs to. */
	uint32_t cached_node;
	/** @brief Position of @c cached_fd. */
	uint64_t cached_position;
	/** @brief Head of a directory entry group cut by a sector edge, until the next sector. */
	uint8_t split_head[19U * 32U];
	/** @brief Directory parsing scratch, kept out of the USB task's small stack. */
	uint8_t directory_window[2U * PROXY_SECTOR_SIZE + 19U * 32U];
	/** @brief Contents of the sector before the one being written. */
	uint8_t directory_previous[PROXY_SECTOR_SIZE];
	/** @brief Parent of the entry group cut by @c split_offset. */
	uint32_t split_parent;
	/** @brief Offset of the sector holding the cut entry group. */
	uint64_t split_offset;
	/** @brief Number of slots of the cut group kept in @c split_head. */
	uint32_t split_slots;
	/** @brief True when the view is started. */
	bool running;
	/** @brief True when no write or delete is pending. */
	bool clean;
	/** @brief True once the block device callbacks may run. */
	volatile bool armed;
	/** @brief Block device callbacks currently in flight. */
	volatile uint32_t callbacks;
	/** @brief True after the source card was removed; its volumes may only be torn down. */
	volatile bool source_lost;
	/** @brief True when the view refuses writes. */
	bool read_only;
	/** @brief True when an index limit was reached and part of the tree is missing. */
	bool truncated;
	/** @brief True once clusters are being handed out; cleared while a node is added unallocated. */
	bool layout_ready;
};

/**
 * @brief Accumulated cost of one operation, in calls and microseconds.
 *
 * Tells a slow card from slow bookkeeping.
 */
struct proxy_timing {
	/** @brief Number of calls measured. */
	uint64_t calls;
	/** @brief Total time spent, in microseconds. */
	uint64_t microseconds;
};

/** @brief Slots in a directory window before an entry group is added. */
#define PROXY_WINDOW_SLOTS (3U * PROXY_SECTOR_SIZE / 32U)

/**
 * @brief Counters and timings reported by the storageproxy status command.
 */
struct proxy_statistics {
	/** @brief Time spent in raw disk writes. */
	struct proxy_timing disk_write;
	/** @brief Time spent flushing the batch buffer. */
	struct proxy_timing batch_flush;
	/** @brief Time spent parsing directory writes. */
	struct proxy_timing directory_write;
	/** @brief Time spent queueing pending sectors. */
	struct proxy_timing pending_store;
	/** @brief Time spent flushing pending sectors. */
	struct proxy_timing pending_flush;
	/** @brief Time spent syncing one directory entry. */
	struct proxy_timing sync_entry;
	/** @brief Sectors written by the host. */
	uint64_t sectors;
	/** @brief Bytes written to source files. */
	uint64_t file_bytes;
	/** @brief High-water mark of @c pending_count. */
	uint32_t pending_peak;
	/** @brief Number of failed writes, capped by the logging. */
	uint32_t write_errors;
	/** @brief Sectors moved into staging runs. */
	uint64_t staged_sectors;
	/** @brief Staging runs that became the file by rename. */
	uint32_t runs_renamed;
	/** @brief Staging stretches copied into an existing file. */
	uint32_t runs_copied;
	/** @brief Number of entry syncs traced to the log. */
	uint32_t sync_trace;
	/** @brief Stage reached by the last entry sync, for failure diagnosis. */
	uint32_t stage;
	/** @brief Last error an entry sync returned. */
	int last_sync_error;
	/** @brief Times the node index reached its configured limit. */
	uint32_t index_full;
	/** @brief Times the string arena ran out. */
	uint32_t strings_full;
	/** @brief Times a directory exceeded its advertised slot capacity. */
	uint32_t dir_capacity_exceeded;
};

/* The single view and its counters, defined in proxy_core.c. */
/** @brief The one view instance. */
extern struct storage_proxy proxy;
/** @brief Counters and timings of @c proxy. */
extern struct proxy_statistics proxy_stats;

/**
 * @brief Enter a block device callback, refusing new ones once stop began.
 *
 * @return True when the callback may run; the caller must call
 *         @ref proxy_armed_leave() before returning.
 */
static inline bool proxy_armed_enter(void)
{
	if (!__atomic_load_n(&proxy.armed, __ATOMIC_SEQ_CST)) {
		return false;
	}
	(void)__atomic_add_fetch(&proxy.callbacks, 1U, __ATOMIC_SEQ_CST);
	return true;
}

/**
 * @brief Leave a block device callback entered with @ref proxy_armed_enter().
 */
static inline void proxy_armed_leave(void)
{
	(void)__atomic_sub_fetch(&proxy.callbacks, 1U, __ATOMIC_SEQ_CST);
}
/** @brief Block device operations the view registers under @c PROXY_DISK_NAME. */
extern const struct block_device_operations proxy_disk_operations;

/**
 * @brief Resolve a string arena offset to its C string.
 *
 * @param[in] offset Offset previously returned by @c proxy_store_string().
 * @return Pointer into the arena.
 */
static inline const char *proxy_string(uint32_t offset)
{
	return proxy.strings + offset;
}

/**
 * @brief Allocate memory for the view from the kernel heap.
 *
 * @param[in] size Bytes to allocate.
 * @return The allocation, or NULL.
 */
void *proxy_alloc(size_t size);
/**
 * @brief Release memory obtained from @c proxy_alloc().
 *
 * @param[in] pointer Allocation to free, or NULL.
 */
void proxy_free(void *pointer);
/**
 * @brief Allocate zeroed memory for the view from the kernel heap.
 *
 * @param[in] count Number of elements.
 * @param[in] size  Size of one element.
 * @return The zeroed allocation, or NULL.
 */
void *proxy_calloc(size_t count, size_t size);

/**
 * @brief Mark the start of a timed operation.
 *
 * @return Timestamp to pass to @c proxy_time_end().
 */
static inline uint64_t proxy_time_begin(void)
{
	return ktime_get_us();
}

/**
 * @brief Add one measured operation to a timing counter.
 *
 * @param[in,out] timing Counter to update.
 * @param[in]     begin  Timestamp from @c proxy_time_begin().
 */
static inline void proxy_time_end(struct proxy_timing *timing, uint64_t begin)
{
	timing->calls++;
	timing->microseconds += ktime_get_us() - begin;
}

/**
 * @brief Store a 16-bit value little endian.
 *
 * @param[out] destination Two bytes to write.
 * @param[in]  value       Value to store.
 */
static inline void put_le16(void *destination, uint16_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = value;
	bytes[1] = value >> 8;
}

/**
 * @brief Store a 32-bit value little endian.
 *
 * @param[out] destination Four bytes to write.
 * @param[in]  value       Value to store.
 */
static inline void put_le32(void *destination, uint32_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = value;
	bytes[1] = value >> 8;
	bytes[2] = value >> 16;
	bytes[3] = value >> 24;
}

/**
 * @brief Store a 64-bit value little endian.
 *
 * @param[out] destination Eight bytes to write.
 * @param[in]  value       Value to store.
 */
static inline void put_le64(void *destination, uint64_t value)
{
	uint8_t *bytes = destination;

	put_le32(bytes, (uint32_t)value);
	put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

/**
 * @brief Load a 16-bit value little endian.
 *
 * @param[in] source Two bytes to read.
 * @return The loaded value.
 */
static inline uint16_t get_le16(const void *source)
{
	const uint8_t *bytes = source;

	return bytes[0] | (uint16_t)bytes[1] << 8;
}

/**
 * @brief Load a 32-bit value little endian.
 *
 * @param[in] source Four bytes to read.
 * @return The loaded value.
 */
static inline uint32_t get_le32(const void *source)
{
	const uint8_t *bytes = source;

	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/**
 * @brief Load a 64-bit value little endian.
 *
 * @param[in] source Eight bytes to read.
 * @return The loaded value.
 */
static inline uint64_t get_le64(const void *source)
{
	const uint8_t *bytes = source;

	return get_le32(bytes) | (uint64_t)get_le32(bytes + 4U) << 32;
}


#endif /* CARDSIGHT_STORAGE_EAGER_PROXY_TYPES_H */
