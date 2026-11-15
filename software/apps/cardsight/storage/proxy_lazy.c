// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_lazy.c
 * @brief Lazy exFAT proxy: reservations, directory extents, cached indexes and writes.
 *
 * The engine synthesises a second exFAT volume, @c cardview1, on top of the
 * partitions of a source disk. Startup only probes and mounts the selected
 * partitions, computes one reservation region per partition and builds the
 * boot region, FAT, allocation bitmap, up-case table and root directory. No
 * source directory is scanned.
 *
 * The host sees one folder per selected partition. The first read of a view
 * directory makes the engine count and index its source directory, allocates a
 * contiguous directory extent and a contiguous file-data block from the
 * partition reservation, and keeps the resulting entries in a bounded LRU
 * cache. The allocation bitmap is never changed after the volume is
 * registered: everything the view may ever hand out is marked used at startup,
 * and only clusters after the reservations are free.
 *
 * A view cluster is resolved through a sorted range index. A directory range
 * renders cached entries; a data range identifies the owning directory and a
 * binary search over the directory's first-cluster order finds the file.
 *
 * The view is read-write. Directory sectors the host writes are parsed and
 * applied to the source file systems, file data is written through to the
 * source file, host-created objects are recorded in a bounded overlay so they
 * survive cache eviction, and data that arrives before its directory entry
 * waits in a bounded pending table. System areas stay read-only.
 */

#include "proxy_lazy.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <init.h>
#include <log.h>

#include <hal/blkdev.h>
#include <shell.h>
#include <usb/mass_storage.h>
#include <vfs.h>

#include "ff.h"

#ifdef STORAGE_PROXY_HOST_TEST
/** @brief Host-test stand-in for the kernel mutex. */
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

/** @brief Block device name of the lazy view. */
#define PROXY_LAZY_DISK_NAME "cardview1"
/** @brief Logical and physical sector size of the view, in bytes. */
#define PROXY_LAZY_SECTOR_SIZE 512U
/** @brief Sectors per cluster of the synthesized volume. */
#define PROXY_LAZY_SECTORS_PER_CLUSTER 64U
/** @brief Cluster size of the view in bytes (32 KiB). */
#define PROXY_LAZY_CLUSTER_SIZE (PROXY_LAZY_SECTOR_SIZE * PROXY_LAZY_SECTORS_PER_CLUSTER)
/** @brief Number of boot region sectors before the FAT. */
#define PROXY_LAZY_BOOT_SECTORS 24U
/** @brief Sector where the FAT starts. */
#define PROXY_LAZY_FAT_OFFSET 32U
/** @brief First real cluster number of the view. */
#define PROXY_LAZY_ROOT_CLUSTER_MIN 2U
/** @brief FAT entry value that ends a chain. */
#define PROXY_LAZY_EOC 0xffffffffU
/** @brief Identifier that names no directory. */
#define PROXY_LAZY_INVALID_DIR UINT32_MAX
/** @brief Identifier of the synthesized root directory. */
#define PROXY_LAZY_ROOT_ID 0U
/** @brief Number of source partitions the view can expose. */
#define PROXY_LAZY_PARTITIONS STORAGE_PROXY_MAX_PARTITIONS
/** @brief Deepest source directory path the engine rebuilds, in components. */
#define PROXY_LAZY_MAX_DEPTH 64U
/** @brief Longest a block device callback waits for the view lock, in milliseconds. */
#define PROXY_LAZY_LOCK_TIMEOUT_MS 2000
/** @brief Number of clusters one directory cluster holds, at 32 bytes per slot. */
#define PROXY_LAZY_SLOTS_PER_CLUSTER (PROXY_LAZY_CLUSTER_SIZE / 32U)
/** @brief Slots in one view sector. */
#define PROXY_LAZY_SLOTS_PER_SECTOR (PROXY_LAZY_SECTOR_SIZE / 32U)
/** @brief Number of sectors in one view cluster. */
#define PROXY_LAZY_SECTORS_PER_CLUSTER_U32 ((uint32_t)PROXY_LAZY_SECTORS_PER_CLUSTER)
/** @brief Slots the root directory needs before any partition folder. */
#define PROXY_LAZY_ROOT_SYSTEM_SLOTS 3U

#ifndef CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS
/** @brief Directory headers the view can keep, including the root and partition folders. */
#define CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS 4096U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES
/** @brief Arena holding persistent directory names, in bytes. */
#define CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES 262144U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES
/** @brief Directory entries the LRU caches may hold together. */
#define CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES 8192U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES
/** @brief Bytes the LRU caches may hold together, names and order arrays included. */
#define CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES 524288U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES
/** @brief Entries a single source directory may expose before it is truncated. */
#define CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES 16384U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_FILE_ALLOWANCE
/** @brief Clusters reserved for file entries a FAT volume cannot count as inodes. */
#define CONFIG_STORAGE_PROXY_LAZY_FILE_ALLOWANCE 1024U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS
/** @brief Largest free tail the host is shown, in 32 KiB clusters. */
#define CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS 4096U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX
/** @brief Host-changed entries the view remembers across cache eviction. */
#define CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX 1024U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_OVERLAY_NAME_BYTES
/** @brief Arena holding names of overlay records, in bytes. */
#define CONFIG_STORAGE_PROXY_LAZY_OVERLAY_NAME_BYTES 65536U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS
/** @brief Sectors of host data that may wait for their directory entry. */
#define CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS 512U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES
/** @brief FAT entries the view remembers for fragmented host extents. */
#define CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES 4096U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_MAX_RANGES
/** @brief Sorted cluster ranges the view can hold. */
#define CONFIG_STORAGE_PROXY_LAZY_MAX_RANGES 16384U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_DIR_SLACK_CLUSTERS
/** @brief Extra clusters reserved after a directory extent for host growth. */
#define CONFIG_STORAGE_PROXY_LAZY_DIR_SLACK_CLUSTERS 1U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_DIR_ENTRY_SLACK
/** @brief Entry slots a directory cache starts with beyond the counted entries. */
#define CONFIG_STORAGE_PROXY_LAZY_DIR_ENTRY_SLACK 64U
#endif

/** @brief Slot value that names no entry. */
#define PROXY_LAZY_INVALID_SLOT UINT32_MAX
/** @brief Slots a directory write window can hold: one sector plus a cut group head. */
#define PROXY_LAZY_WINDOW_SLOTS (PROXY_LAZY_SLOTS_PER_SECTOR + 18U)

/**
 * @brief What a view cluster range holds.
 */
enum proxy_lazy_kind {
	/** @brief A directory extent that renders entries. */
	PROXY_LAZY_RANGE_DIRECTORY,
	/** @brief A contiguous file-data block owned by one directory. */
	PROXY_LAZY_RANGE_DATA,
};

/**
 * @brief One cached object of a source directory.
 */
struct proxy_lazy_dirent {
	/** @brief File size in bytes; unused for directories. */
	uint64_t size;
	/** @brief First cluster of the view extent. */
	uint32_t first_cluster;
	/** @brief Number of contiguous clusters of the extent. */
	uint32_t clusters;
	/** @brief Absolute first slot inside the view directory. */
	uint32_t slot;
	/** @brief Offset of the name in the owning cache's name arena. */
	uint32_t name_offset;
	/** @brief Number of slots the entry group occupies. */
	uint16_t entries;
	/** @brief True when the object is a directory. */
	bool directory;
	/** @brief True when the host deleted the entry; its slots stay reserved. */
	bool deleted;
	/** @brief True when the extent was assigned by the host. */
	bool host_owned;
	/** @brief True when the host extent is a FAT chain rather than contiguous. */
	bool fat_chain;
};

/**
 * @brief Persistent header of one view directory.
 *
 * The header survives cache eviction so a later reload reproduces the same
 * slots and extents. Only the entry cache, the name arena and the
 * first-cluster order array belong to the LRU and are freed on eviction.
 */
struct proxy_lazy_dir {
	/** @brief Parent directory identifier, or PROXY_LAZY_INVALID_DIR. */
	uint32_t parent;
	/** @brief Partition index the directory belongs to. */
	uint32_t partition;
	/** @brief First cluster of the directory extent, or zero before reservation. */
	uint32_t first_cluster;
	/** @brief Number of clusters the directory extent spans. */
	uint32_t clusters;
	/** @brief First cluster of the directory's contiguous file-data block. */
	uint32_t data_first_cluster;
	/** @brief Number of clusters in the file-data block. */
	uint32_t data_clusters;
	/** @brief Entries the directory planned when its extent was reserved. */
	uint32_t planned_entries;
	/** @brief Slots planned, including the end-of-directory slot. */
	uint32_t planned_slots;
	/** @brief Name bytes planned by the count pass. */
	uint32_t planned_name_bytes;
	/** @brief Number of entries in the cache. */
	uint32_t entry_count;
	/** @brief Allocated capacity of @c entries. */
	uint32_t entry_capacity;
	/** @brief Cached entry array, sorted by slot. */
	struct proxy_lazy_dirent *entries;
	/** @brief Name arena of the cached entries. */
	char *names;
	/** @brief Allocated capacity of @c names. */
	uint32_t name_capacity;
	/** @brief Bytes used in @c names. */
	uint32_t name_used;
	/** @brief Last cluster of the directory's logical extent chain. */
	uint32_t extension_cluster;
	/** @brief Child directory identifiers in discovery order. */
	uint32_t *subdirs;
	/** @brief Number of child directory identifiers. */
	uint32_t subdir_count;
	/** @brief Allocated capacity of @c subdirs. */
	uint32_t subdir_capacity;
	/** @brief Offset of the directory's own name in the global arena. */
	uint32_t name_offset;
	/** @brief Bytes the cache holds, for the LRU accounting. */
	uint32_t cache_bytes;
	/** @brief Previous entry in the LRU list, or PROXY_LAZY_INVALID_DIR. */
	uint32_t lru_prev;
	/** @brief Next entry in the LRU list, or PROXY_LAZY_INVALID_DIR. */
	uint32_t lru_next;
	/** @brief Source error that made this directory render as empty. */
	int source_error;
	/** @brief True for the synthesized root directory. */
	bool is_root;
	/** @brief True for a folder that represents a source partition. */
	bool is_partition_root;
	/** @brief True once the directory extent is reserved. */
	bool extent_ready;
	/** @brief True once the directory extent is in the range index. */
	bool range_ready;
	/** @brief True once the entry cache is populated. */
	bool cache_ready;
	/** @brief True once the file-data block is reserved. */
	bool data_ready;
	/** @brief True once the file-data block is in the range index. */
	bool data_range_ready;
	/** @brief True when a host change made the data ranges stale. */
	bool ranges_dirty;
	/** @brief True when a source file may be longer than its entry says. */
	bool sizes_dirty;
	/** @brief True once the source directory had more entries than the cap. */
	bool truncated;
	/** @brief True when the directory extent is a FAT chain, not contiguous. */
	bool chained;
};

/**
 * @brief One selected source partition and its reservation region.
 */
struct proxy_lazy_partition {
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
	/** @brief First cluster reserved for the partition. */
	uint32_t reserve_first;
	/** @brief Number of clusters reserved for the partition. */
	uint32_t reserve_clusters;
	/** @brief Next unallocated cluster inside the reservation. */
	uint32_t bump;
	/** @brief Directory identifier of the partition folder. */
	uint32_t dir_id;
	/** @brief Probe or mount result. */
	int status;
	/** @brief MBR partition type. */
	uint8_t type;
	/** @brief True when the partition device exists. */
	bool present;
	/** @brief True when the partition was selected for the view. */
	bool selected;
	/** @brief True while the partition is mounted as a source. */
	bool mounted;
};

/**
 * @brief A sorted cluster range that belongs to one directory.
 */
struct proxy_lazy_range {
	/** @brief First cluster of the range. */
	uint32_t base;
	/** @brief Number of contiguous clusters. */
	uint32_t clusters;
	/** @brief Directory that owns the range. */
	uint32_t dir_id;
	/** @brief One of @ref proxy_lazy_kind. */
	uint32_t kind;
	/** @brief Entry slot this data range belongs to, or PROXY_LAZY_INVALID_SLOT. */
	uint32_t slot;
	/** @brief Logical cluster offset of the range inside its directory or file. */
	uint32_t offset;
};

/**
 * @brief One entry of the view root, a partition folder.
 */
struct proxy_lazy_root_entry {
	/** @brief Directory identifier of the folder. */
	uint32_t dir_id;
	/** @brief Absolute first slot of the folder entry. */
	uint32_t slot;
	/** @brief Slots the entry group occupies. */
	uint32_t entries;
};

/**
 * @brief One host-changed directory entry that must survive cache eviction.
 */
struct proxy_lazy_overlay {
	/** @brief Directory identifier. */
	uint32_t dir_id;
	/** @brief Absolute first slot of the entry group. */
	uint32_t slot;
	/** @brief First cluster of the host extent, or zero. */
	uint32_t first_cluster;
	/** @brief Number of clusters of the host extent. */
	uint32_t clusters;
	/** @brief File size in bytes, or the directory data length. */
	uint64_t size;
	/** @brief Offset of the name in the overlay name arena. */
	uint32_t name_offset;
	/** @brief Slots the entry group occupies. */
	uint16_t entries;
	/** @brief True when the record is in use. */
	bool used;
	/** @brief True for a directory. */
	bool directory;
	/** @brief True when the host deleted the entry; the record is a tombstone. */
	bool deleted;
	/** @brief True once the deferred source delete was carried out. */
	bool applied;
	/** @brief True when the host extent is a FAT chain. */
	bool fat_chain;
};

/**
 * @brief One host sector that arrived before the entry that owns it.
 */
struct proxy_lazy_pending_sector {
	/** @brief View cluster the sector belongs to. */
	uint32_t cluster;
	/** @brief Sector offset inside the cluster. */
	uint16_t sector;
	/** @brief True while the sector holds data. */
	bool used;
	/** @brief Sector contents. */
	uint8_t data[PROXY_LAZY_SECTOR_SIZE];
};

/**
 * @brief One FAT entry the host wrote for a fragmented extent.
 */
struct proxy_lazy_fat_entry {
	/** @brief Cluster number. */
	uint32_t cluster;
	/** @brief Next cluster of the chain, or an end-of-chain value. */
	uint32_t next;
	/** @brief True while the entry is in use. */
	bool used;
};

/**
 * @brief The lazy view and every buffer it holds.
 */
struct proxy_lazy {
	/** @brief Selected source partitions. */
	struct proxy_lazy_partition partitions[PROXY_LAZY_PARTITIONS];
	/** @brief Persistent directory headers. */
	struct proxy_lazy_dir *dirs;
	/** @brief Number of directory headers in use. */
	uint32_t dir_count;
	/** @brief Allocated capacity of @c dirs. */
	uint32_t dir_capacity;
	/** @brief Persistent arena holding directory names. */
	char *name_arena;
	/** @brief Bytes used in @c name_arena. */
	uint32_t name_used;
	/** @brief Sorted range index of directory extents and file-data blocks. */
	struct proxy_lazy_range *ranges;
	/** @brief Number of ranges in use. */
	uint32_t range_count;
	/** @brief Allocated capacity of @c ranges. */
	uint32_t range_capacity;
	/** @brief Allocation bitmap of the view, one bit per cluster. */
	uint8_t *bitmap;
	/** @brief Useful bytes of the allocation bitmap. */
	uint32_t bitmap_bytes;
	/** @brief Number of clusters the allocation bitmap spans. */
	uint32_t bitmap_clusters;
	/** @brief Up-case table, two bytes per UTF-16 code unit. */
	uint8_t *upcase;
	/** @brief Useful bytes of the up-case table. */
	uint32_t upcase_bytes;
	/** @brief First cluster of the up-case table. */
	uint32_t upcase_cluster;
	/** @brief Number of clusters the up-case table spans. */
	uint32_t upcase_clusters;
	/** @brief Checksum stored in the up-case entry and the boot sector. */
	uint32_t upcase_checksum;
	/** @brief Number of clusters in the view. */
	uint32_t total_clusters;
	/** @brief Total sectors of the view. */
	uint64_t total_sectors;
	/** @brief First data sector of the view. */
	uint32_t data_start;
	/** @brief FAT sectors in the view. */
	uint32_t fat_sectors;
	/** @brief First cluster of the allocation bitmap. */
	uint32_t bitmap_cluster;
	/** @brief First cluster of the root directory. */
	uint32_t root_cluster;
	/** @brief Number of clusters the root directory spans. */
	uint32_t root_clusters;
	/** @brief First free cluster after every reservation. */
	uint32_t tail_first;
	/** @brief Number of free clusters the host sees as the tail. */
	uint32_t tail_clusters;
	/** @brief Root entries that represent partition folders and host helpers. */
	struct proxy_lazy_root_entry root_entries[PROXY_LAZY_PARTITIONS + 1U];
	/** @brief Number of entries in @c root_entries. */
	uint32_t root_entry_count;
	/** @brief First root slot reserved for the host; system and folder slots stay read-only. */
	uint32_t root_host_slot;
	/** @brief Host-changed entries that survive cache eviction. */
	struct proxy_lazy_overlay *overlay;
	/** @brief Number of live overlay records. */
	uint32_t overlay_count;
	/** @brief Bytes used in @c overlay_names. */
	uint32_t overlay_name_used;
	/** @brief Arena holding the names of overlay records. */
	char *overlay_names;
	/** @brief Host sectors waiting for their directory entry. */
	struct proxy_lazy_pending_sector *pending;
	/** @brief Number of live pending sectors. */
	uint32_t pending_count;
	/** @brief FAT entries the host wrote for fragmented extents. */
	struct proxy_lazy_fat_entry *fat_log;
	/** @brief Number of live FAT log entries. */
	uint32_t fat_count;
	/** @brief Window used to reassemble a group cut by a sector edge. */
	uint8_t directory_window[PROXY_LAZY_WINDOW_SLOTS * 32U];
	/** @brief Previous contents of the directory sector being written. */
	uint8_t directory_previous[PROXY_LAZY_SECTOR_SIZE];
	/** @brief Head slots of a group cut by the previous directory write. */
	uint8_t split_head[18U * 32U];
	/** @brief Slots stored in @c split_head. */
	uint32_t split_slots;
	/** @brief Directory @c split_head belongs to. */
	uint32_t split_dir;
	/** @brief Byte offset of the sector that stored @c split_head. */
	uint64_t split_offset;
	/** @brief Cached write descriptor, or -1. */
	int write_fd;
	/** @brief Directory @c write_fd belongs to. */
	uint32_t write_dir;
	/** @brief Slot of the file @c write_fd belongs to. */
	uint32_t write_slot;
	/** @brief Position of @c write_fd. */
	uint64_t write_position;
	/** @brief Block device name of the source disk. */
	char source_name[VFS_BLK_NAME];
	/** @brief Block device the view is registered as. */
	struct gendisk *disk;
	/** @brief Lock held by every block device callback and every start/stop. */
	struct mutex lock;
	/** @brief Cached read descriptor, or -1. */
	int cached_fd;
	/** @brief Directory @c cached_fd belongs to. */
	uint32_t cached_dir;
	/** @brief Slot of the file @c cached_fd belongs to. */
	uint32_t cached_slot;
	/** @brief Position of @c cached_fd. */
	uint64_t cached_position;
	/** @brief Entries currently cached. */
	uint32_t cache_entries;
	/** @brief Bytes currently cached. */
	uint32_t cache_bytes;
	/** @brief Least recently used cached directory, or PROXY_LAZY_INVALID_DIR. */
	uint32_t lru_head;
	/** @brief Most recently used cached directory, or PROXY_LAZY_INVALID_DIR. */
	uint32_t lru_tail;
	/** @brief True when the view is started. */
	bool running;
	/** @brief True while this view is the device exported over USB. */
	bool usb_active;
	/** @brief True once the block device callbacks may run. */
	volatile bool armed;
	/** @brief Block device callbacks currently in flight. */
	volatile uint32_t callbacks;
	/** @brief True when an index limit was reached and part of the tree is missing. */
	bool truncated;
	/** @brief Last error a directory operation reported. */
	int last_error;
	/** @brief Number of entry caches evicted so far. */
	uint32_t evictions;
	/** @brief Number of sectors the host read. */
	uint64_t sectors_read;
	/** @brief Number of sectors the host wrote. */
	uint64_t sectors_written;
	/** @brief Number of source file bytes the view served. */
	uint64_t file_bytes;
	/** @brief Number of source file bytes the view accepted. */
	uint64_t file_bytes_written;
	/** @brief True when nothing is waiting to reach a source volume. */
	bool clean;
	/** @brief True when the last flush failed. */
	bool flush_failed;
};

/** @brief The one lazy view instance. */
static struct proxy_lazy lazy;
/** @brief Entry array the first-cluster comparator sorts, set under the lock. */
static const struct proxy_lazy_dirent *lazy_sort_entries;

/** @brief Resize a source file; defined with the write path below. */
static int lazy_truncate_entry(uint32_t dir_id, uint32_t entry_index, uint64_t size);

/* ---- small helpers ---- */

/**
 * @brief Allocate memory for the view.
 *
 * @param[in] size Bytes to allocate.
 * @return The allocation, or NULL.
 */
static void *lazy_alloc(size_t size)
{
#ifdef STORAGE_PROXY_HOST_TEST
	return malloc(size);
#else
	return pvPortMalloc(size);
#endif
}

/**
 * @brief Release memory obtained from @c lazy_alloc().
 *
 * @param[in] pointer Allocation to free, or NULL.
 */
static void lazy_free(void *pointer)
{
	if (pointer == NULL) {
		return;
	}
#ifdef STORAGE_PROXY_HOST_TEST
	free(pointer);
#else
	vPortFree(pointer);
#endif
}

/**
 * @brief Allocate zeroed memory for the view.
 *
 * @param[in] count Number of elements.
 * @param[in] size  Size of one element.
 * @return The zeroed allocation, or NULL.
 */
static void *lazy_calloc(size_t count, size_t size)
{
	void *pointer;

	if (count != 0U && size > SIZE_MAX / count) {
		return NULL;
	}
	pointer = lazy_alloc(count * size);
	if (pointer != NULL) {
		memset(pointer, 0, count * size);
	}
	return pointer;
}

/**
 * @brief Store a 16-bit value little endian.
 *
 * @param[out] destination Two bytes to write.
 * @param[in]  value       Value to store.
 */
static inline void lazy_put_le16(void *destination, uint16_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/**
 * @brief Store a 32-bit value little endian.
 *
 * @param[out] destination Four bytes to write.
 * @param[in]  value       Value to store.
 */
static inline void lazy_put_le32(void *destination, uint32_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/**
 * @brief Store a 64-bit value little endian.
 *
 * @param[out] destination Eight bytes to write.
 * @param[in]  value       Value to store.
 */
static inline void lazy_put_le64(void *destination, uint64_t value)
{
	uint8_t *bytes = destination;

	lazy_put_le32(bytes, (uint32_t)value);
	lazy_put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

/**
 * @brief Fold one byte into the exFAT rotating 32-bit checksum.
 *
 * @param[in] checksum Current checksum.
 * @param[in] value    Byte to add.
 * @return The updated checksum.
 */
static uint32_t lazy_checksum32(uint32_t checksum, uint8_t value)
{
	return ((checksum & 1U) != 0U ? 0x80000000U : 0U) + (checksum >> 1) + value;
}

/**
 * @brief Enter a block device callback, refusing new ones once stop began.
 *
 * @return True when the callback may run; the caller must call
 *         @c lazy_armed_leave() before returning.
 */
static inline bool lazy_armed_enter(void)
{
	if (!__atomic_load_n(&lazy.armed, __ATOMIC_SEQ_CST)) {
		return false;
	}
	(void)__atomic_add_fetch(&lazy.callbacks, 1U, __ATOMIC_SEQ_CST);
	return true;
}

/**
 * @brief Leave a block device callback entered with @c lazy_armed_enter().
 */
static inline void lazy_armed_leave(void)
{
	(void)__atomic_sub_fetch(&lazy.callbacks, 1U, __ATOMIC_SEQ_CST);
}

/* ---- names and persistent arena ---- */

/**
 * @brief Copy a directory name into the persistent arena.
 *
 * @param[in] name NUL-terminated name to copy.
 * @return Offset in the arena, or UINT32_MAX when the arena is full.
 */
static uint32_t lazy_store_name(const char *name)
{
	size_t length = strlen(name) + 1U;
	uint32_t offset;

	if (lazy.name_arena == NULL || length > CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES - lazy.name_used) {
		return UINT32_MAX;
	}
	offset = lazy.name_used;
	memcpy(lazy.name_arena + offset, name, length);
	lazy.name_used += (uint32_t)length;
	return offset;
}

/**
 * @brief Resolve the persistent name of a directory.
 *
 * @param[in] identifier Directory identifier.
 * @return Pointer into the persistent name arena.
 */
static const char *lazy_dir_name(uint32_t identifier)
{
	return lazy.name_arena + lazy.dirs[identifier].name_offset;
}

/**
 * @brief Convert a UTF-8 string to UTF-16 for an exFAT name.
 *
 * @details Decodes one code point at a time, replacing malformed sequences
 * with U+FFFD. Code points above the BMP are written as a surrogate pair, and
 * the conversion stops when the output capacity would be exceeded.
 *
 * @param[in]  input    NUL-terminated UTF-8 string.
 * @param[out] output   Buffer receiving UTF-16 code units.
 * @param[in]  capacity Number of code units @p output can hold.
 * @return Number of code units written.
 */
static size_t lazy_utf16(const char *input, uint16_t *output, size_t capacity)
{
	size_t length = 0U;

	while (*input != '\0' && length < capacity) {
		uint32_t codepoint;
		unsigned char first = (unsigned char)*input++;

		if (first < 0x80U) {
			codepoint = first;
		} else if ((first & 0xe0U) == 0xc0U && ((unsigned char)input[0] & 0xc0U) == 0x80U) {
			codepoint = ((uint32_t)(first & 0x1fU) << 6) | ((unsigned char)*input++ & 0x3fU);
		} else if ((first & 0xf0U) == 0xe0U && ((unsigned char)input[0] & 0xc0U) == 0x80U &&
			   ((unsigned char)input[1] & 0xc0U) == 0x80U) {
			codepoint = ((uint32_t)(first & 0x0fU) << 12) |
				    ((uint32_t)((unsigned char)input[0] & 0x3fU) << 6) |
				    ((unsigned char)input[1] & 0x3fU);
			input += 2;
		} else if ((first & 0xf8U) == 0xf0U && ((unsigned char)input[0] & 0xc0U) == 0x80U &&
			   ((unsigned char)input[1] & 0xc0U) == 0x80U &&
			   ((unsigned char)input[2] & 0xc0U) == 0x80U) {
			codepoint = ((uint32_t)(first & 0x07U) << 18) |
				    ((uint32_t)((unsigned char)input[0] & 0x3fU) << 12) |
				    ((uint32_t)((unsigned char)input[1] & 0x3fU) << 6) |
				    ((unsigned char)input[2] & 0x3fU);
			input += 3;
		} else {
			codepoint = 0xfffdU;
		}
		if (codepoint > 0xffffU) {
			if (length + 2U > capacity) {
				break;
			}
			codepoint -= 0x10000U;
			output[length++] = (uint16_t)(0xd800U | (codepoint >> 10));
			output[length++] = (uint16_t)(0xdc00U | (codepoint & 0x3ffU));
		} else {
			output[length++] = (uint16_t)codepoint;
		}
	}
	return length;
}

/**
 * @brief Number of 32-byte slots a name occupies.
 *
 * @param[in] name UTF-8 name.
 * @return Slot count, including the stream and name entries.
 */
static uint32_t lazy_name_entries(const char *name)
{
	uint16_t characters[VFS_NAME_MAX + 1];
	size_t length = lazy_utf16(name, characters, VFS_NAME_MAX);

	return 2U + (uint32_t)((length + 14U) / 15U);
}

/**
 * @brief Compute the exFAT name hash stored in a stream entry.
 *
 * @param[in] name   UTF-16 name.
 * @param[in] length Number of code units in @p name.
 * @return The name hash.
 */
static uint16_t lazy_name_hash(const uint16_t *name, size_t length)
{
	uint16_t checksum = 0U;

	for (size_t index = 0U; index < length; index++) {
		uint16_t character = (uint16_t)ff_wtoupper(name[index]);

		checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
			(checksum >> 1) + (uint8_t)character);
		checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
			(checksum >> 1) + (uint8_t)(character >> 8));
	}
	return checksum;
}

/* ---- up-case table ---- */

/**
 * @brief Build the up-case table the view advertises.
 *
 * @details Each UTF-16 code unit is mapped through the source's upper-case
 * conversion. Runs of 128 or more units that map to themselves are compressed
 * into the exFAT identity-run encoding (0xFFFF followed by the run length). The
 * checksum is computed over the bytes exactly as they are stored.
 *
 * @return 0 on success, or -ENOMEM.
 */
static int lazy_build_upcase(void)
{
	uint16_t source = 0U;
	uint16_t character;
	uint32_t state = 0U;
	uint32_t run = 0U;
	uint32_t checksum = 0U;
	size_t offset = 0U;

	lazy.upcase = lazy_alloc(65536U * sizeof(uint16_t));
	if (lazy.upcase == NULL) {
		return -ENOMEM;
	}
	do {
		switch (state) {
		case 0:
			character = (uint16_t)ff_wtoupper(source);
			if (character != source) {
				source++;
				break;
			}
			for (run = 1U; (uint16_t)(source + run) != 0U &&
			     (uint16_t)(source + run) == (uint16_t)ff_wtoupper((uint16_t)(source + run)); run++) {
			}
			if (run >= 128U) {
				character = 0xffffU;
				state = 2U;
				break;
			}
			state = 1U;
			/* fall through */
		case 1:
			character = source++;
			if (--run == 0U) {
				state = 0U;
			}
			break;
		default:
			character = (uint16_t)run;
			source = (uint16_t)(source + run);
			state = 0U;
			break;
		}
		lazy.upcase[offset++] = (uint8_t)character;
		lazy.upcase[offset++] = (uint8_t)(character >> 8);
		checksum = lazy_checksum32(checksum, lazy.upcase[offset - 2U]);
		checksum = lazy_checksum32(checksum, lazy.upcase[offset - 1U]);
	} while (source != 0U);
	lazy.upcase_bytes = (uint32_t)offset;
	lazy.upcase_checksum = checksum;
	return 0;
}

/* ---- range index ---- */

/**
 * @brief Grow the sorted range index.
 *
 * @return 0 on success, or -ENOMEM.
 */
static int lazy_range_grow(void)
{
	uint32_t capacity = lazy.range_capacity == 0U ? 32U : lazy.range_capacity * 2U;
	struct proxy_lazy_range *ranges;

	if (capacity > CONFIG_STORAGE_PROXY_LAZY_MAX_RANGES) {
		capacity = CONFIG_STORAGE_PROXY_LAZY_MAX_RANGES;
	}
	if (capacity <= lazy.range_capacity) {
		return -ENOMEM;
	}
	ranges = lazy_alloc((size_t)capacity * sizeof(*ranges));
	if (ranges == NULL) {
		return -ENOMEM;
	}
	if (lazy.ranges != NULL) {
		memcpy(ranges, lazy.ranges, (size_t)lazy.range_count * sizeof(*ranges));
		lazy_free(lazy.ranges);
	}
	lazy.ranges = ranges;
	lazy.range_capacity = capacity;
	return 0;
}

/**
 * @brief Add one cluster range to the sorted index.
 *
 * @details Adjacent ranges of the same owner and logical offset are merged, so
 * one file or one directory costs one range rather than one per cluster.
 *
 * @param[in] base     First cluster of the range.
 * @param[in] clusters Number of clusters; zero adds nothing.
 * @param[in] dir_id   Directory that owns the range.
 * @param[in] kind     One of @ref proxy_lazy_kind.
 * @param[in] slot     Entry slot of a data range, or PROXY_LAZY_INVALID_SLOT.
 * @param[in] offset   Logical cluster offset inside the owner.
 * @return 0 on success, or -ENOMEM.
 */
static int lazy_range_add(uint32_t base, uint32_t clusters, uint32_t dir_id, uint32_t kind,
	uint32_t slot, uint32_t offset)
{
	uint32_t low = 0U;
	uint32_t high = lazy.range_count;

	if (clusters == 0U) {
		return 0;
	}
	if (lazy.range_count == lazy.range_capacity && lazy_range_grow() != 0) {
		return -ENOMEM;
	}
	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;

		if (lazy.ranges[middle].base < base) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	if (low != 0U) {
		struct proxy_lazy_range *previous = &lazy.ranges[low - 1U];

		if (previous->dir_id == dir_id && previous->kind == kind && previous->slot == slot &&
		    previous->base + previous->clusters == base &&
		    previous->offset + previous->clusters == offset) {
			previous->clusters += clusters;
			if (low < lazy.range_count) {
				struct proxy_lazy_range *next = &lazy.ranges[low];

				if (next->dir_id == dir_id && next->kind == kind && next->slot == slot &&
				    previous->base + previous->clusters == next->base &&
				    previous->offset + previous->clusters == next->offset) {
					previous->clusters += next->clusters;
					memmove(next, next + 1U,
						(size_t)(lazy.range_count - low - 1U) * sizeof(*next));
					lazy.range_count--;
				}
			}
			return 0;
		}
	}
	memmove(&lazy.ranges[low + 1U], &lazy.ranges[low],
		(size_t)(lazy.range_count - low) * sizeof(*lazy.ranges));
	lazy.ranges[low].base = base;
	lazy.ranges[low].clusters = clusters;
	lazy.ranges[low].dir_id = dir_id;
	lazy.ranges[low].kind = kind;
	lazy.ranges[low].slot = slot;
	lazy.ranges[low].offset = offset;
	lazy.range_count++;
	return 0;
}

/**
 * @brief Remove every range of one owner and kind.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] kind   One of @ref proxy_lazy_kind.
 */
static void lazy_range_remove_owner(uint32_t dir_id, uint32_t kind)
{
	uint32_t kept = 0U;

	for (uint32_t index = 0U; index < lazy.range_count; index++) {
		struct proxy_lazy_range *range = &lazy.ranges[index];

		if (range->dir_id == dir_id && range->kind == kind) {
			continue;
		}
		if (kept != index) {
			lazy.ranges[kept] = *range;
		}
		kept++;
	}
	lazy.range_count = kept;
}

/**
 * @brief Remove the data range that contains a cluster.
 *
 * @details The host frees clusters through the allocation bitmap; the range of
 * the file that used them must stop claiming them before the host hands them to
 * another object. A partially removed file is rebuilt when its entry is next
 * written.
 *
 * @param[in] cluster Cluster that became free.
 */
static void lazy_range_remove_data_cluster(uint32_t cluster)
{
	uint32_t low = 0U;
	uint32_t high = lazy.range_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const struct proxy_lazy_range *range = &lazy.ranges[middle];

		if (cluster < range->base) {
			high = middle;
		} else if (cluster >= range->base + range->clusters) {
			low = middle + 1U;
		} else {
			/*
			 * A truncate-through-the-bitmap frees the old clusters before the
			 * directory entry is rewritten, and the host may hand the same
			 * cluster straight back to this file. Keep the live entry's range
			 * until its entry is updated or deleted; only drop the extent of a
			 * deleted object, whose clusters a new object may now own.
			 */
			if (range->kind == PROXY_LAZY_RANGE_DATA &&
			    range->slot != PROXY_LAZY_INVALID_SLOT &&
			    range->dir_id < lazy.dir_count) {
				struct proxy_lazy_dir *owner = &lazy.dirs[range->dir_id];
				bool live = false;

				if (owner->cache_ready && owner->entries != NULL) {
					for (uint32_t index = 0U; index < owner->entry_count; index++) {
						struct proxy_lazy_dirent *item = &owner->entries[index];

						if (!item->deleted && item->slot == range->slot &&
						    cluster >= item->first_cluster &&
						    cluster < item->first_cluster + item->clusters) {
							live = true;
							break;
						}
					}
				}
				if (!live) {
					memmove(&lazy.ranges[middle], &lazy.ranges[middle + 1U],
						(size_t)(lazy.range_count - middle - 1U) *
						sizeof(*range));
					lazy.range_count--;
				}
			}
			return;
		}
	}
}

/**
 * @brief Find the range that contains a cluster.
 *
 * @param[in]  cluster Cluster number.
 * @param[out] found   Receives the range.
 * @return 0 when a range was found, or -ENOENT.
 */
static int lazy_range_find(uint32_t cluster, struct proxy_lazy_range *found)
{
	uint32_t low = 0U;
	uint32_t high = lazy.range_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const struct proxy_lazy_range *range = &lazy.ranges[middle];

		if (cluster < range->base) {
			high = middle;
		} else if (cluster >= range->base + range->clusters) {
			low = middle + 1U;
		} else {
			*found = *range;
			return 0;
		}
	}
	return -ENOENT;
}

/* ---- reservation allocation ---- */

/**
 * @brief Hand out contiguous clusters from one partition reservation.
 *
 * @details The allocation bitmap is not touched: the whole reservation was
 * marked used before the volume was registered. Only the partition bump moves.
 *
 * @param[in]  partition Partition index.
 * @param[in]  clusters  Number of clusters; zero yields an empty extent.
 * @param[out] first     Receives the first cluster, or zero.
 * @param[out] count     Receives the cluster count, or zero.
 * @return 0 on success, or -ENOSPC when the reservation is exhausted.
 */
static int lazy_alloc_clusters(uint32_t partition, uint32_t clusters, uint32_t *first, uint32_t *count)
{
	struct proxy_lazy_partition *part;

	if (partition >= PROXY_LAZY_PARTITIONS) {
		return -EINVAL;
	}
	part = &lazy.partitions[partition];
	if (clusters == 0U) {
		*first = 0U;
		*count = 0U;
		return 0;
	}
	if ((uint64_t)part->bump + clusters > (uint64_t)part->reserve_first + part->reserve_clusters) {
		return -ENOSPC;
	}
	*first = part->bump;
	*count = clusters;
	part->bump += clusters;
	return 0;
}

/* ---- directory paths ---- */

/**
 * @brief Rebuild the source path of a view directory.
 *
 * @details The path is the mount point of the owning partition followed by the
 * names of the directories between it and @p identifier. The chain is walked
 * upwards and written backwards, so nothing per-directory path is stored.
 *
 * @param[in]  identifier Directory identifier.
 * @param[out] path       Buffer receiving the path.
 * @param[in]  size       Size of @p path in bytes.
 * @return 0 on success, or a negative errno.
 */
static int lazy_dir_path(uint32_t identifier, char *path, size_t size)
{
	uint32_t chain[PROXY_LAZY_MAX_DEPTH];
	uint32_t depth = 0U;
	uint32_t current = identifier;
	uint32_t partition = UINT32_MAX;
	int written;

	if (identifier >= lazy.dir_count) {
		return -ENOENT;
	}
	while (current != PROXY_LAZY_ROOT_ID) {
		struct proxy_lazy_dir *dir = &lazy.dirs[current];

		if (dir->is_partition_root) {
			partition = dir->partition;
			break;
		}
		if (depth >= PROXY_LAZY_MAX_DEPTH || dir->parent == PROXY_LAZY_INVALID_DIR ||
		    dir->parent == PROXY_LAZY_ROOT_ID) {
			return -ENOENT;
		}
		chain[depth++] = dir->name_offset;
		current = dir->parent;
	}
	if (partition >= PROXY_LAZY_PARTITIONS || lazy.partitions[partition].mount[0] == '\0') {
		return -ENOENT;
	}
	written = snprintf(path, size, "%s", lazy.partitions[partition].mount);
	if (written < 0 || (size_t)written >= size) {
		return -ENAMETOOLONG;
	}
	while (depth != 0U) {
		const char *name = lazy.name_arena + chain[--depth];
		int added = snprintf(path + written, size - (size_t)written, "/%s", name);

		if (added < 0 || (size_t)added >= size - (size_t)written) {
			return -ENAMETOOLONG;
		}
		written += added;
	}
	return 0;
}

/**
 * @brief Rebuild the source path of one cached directory entry.
 *
 * @param[in]  dir_id Directory identifier.
 * @param[in]  item   Cached entry.
 * @param[out] path   Buffer receiving the path.
 * @param[in]  size   Size of @p path in bytes.
 * @return 0 on success, or a negative errno.
 */
static int lazy_entry_path(uint32_t dir_id, const struct proxy_lazy_dirent *item,
	char *path, size_t size)
{
	size_t length;
	int result = lazy_dir_path(dir_id, path, size);
	int added;

	if (result != 0) {
		return result;
	}
	length = strlen(path);
	added = snprintf(path + length, size - length, "/%s", lazy.dirs[dir_id].names + item->name_offset);
	if (added < 0 || (size_t)added >= size - length) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/* ---- directory headers ---- */

/**
 * @brief Allocate one persistent directory header.
 *
 * @param[in] name Directory name.
 * @return The new identifier, or UINT32_MAX when the table or arena is full.
 */
static uint32_t lazy_dir_alloc(const char *name)
{
	uint32_t identifier;
	uint32_t name_offset;

	if (lazy.dir_count >= lazy.dir_capacity) {
		lazy.truncated = true;
		return UINT32_MAX;
	}
	name_offset = lazy_store_name(name);
	if (name_offset == UINT32_MAX) {
		lazy.truncated = true;
		return UINT32_MAX;
	}
	identifier = lazy.dir_count++;
	memset(&lazy.dirs[identifier], 0, sizeof(lazy.dirs[identifier]));
	lazy.dirs[identifier].parent = PROXY_LAZY_INVALID_DIR;
	lazy.dirs[identifier].partition = PROXY_LAZY_PARTITIONS;
	lazy.dirs[identifier].name_offset = name_offset;
	lazy.dirs[identifier].lru_prev = PROXY_LAZY_INVALID_DIR;
	lazy.dirs[identifier].lru_next = PROXY_LAZY_INVALID_DIR;
	return identifier;
}

/**
 * @brief Grow the child-directory list of a header.
 *
 * @param[in,out] dir Directory whose list grows.
 * @return 0 on success, or -ENOMEM.
 */
static int lazy_subdir_reserve(struct proxy_lazy_dir *dir)
{
	uint32_t capacity;
	uint32_t *subdirs;

	if (dir->subdir_count < dir->subdir_capacity) {
		return 0;
	}
	capacity = dir->subdir_capacity == 0U ? 8U : dir->subdir_capacity * 2U;
	if (capacity > CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS) {
		capacity = CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS;
	}
	if (capacity <= dir->subdir_capacity) {
		return -ENOMEM;
	}
	subdirs = lazy_alloc((size_t)capacity * sizeof(*subdirs));
	if (subdirs == NULL) {
		return -ENOMEM;
	}
	if (dir->subdirs != NULL) {
		memcpy(subdirs, dir->subdirs, (size_t)dir->subdir_count * sizeof(*subdirs));
		lazy_free(dir->subdirs);
	}
	dir->subdirs = subdirs;
	dir->subdir_capacity = capacity;
	return 0;
}

/**
 * @brief Register an existing child header under a parent.
 *
 * @param[in,out] parent Parent directory.
 * @param[in]     child  Child identifier.
 * @return 0 on success, or -ENOMEM.
 */
static int lazy_subdir_append(struct proxy_lazy_dir *parent, uint32_t child)
{
	int result = lazy_subdir_reserve(parent);

	if (result != 0) {
		return result;
	}
	parent->subdirs[parent->subdir_count++] = child;
	return 0;
}

/* ---- counting and reserving a directory extent ---- */

/**
 * @brief Count the entries and slots of a source directory.
 *
 * @details One opendir/readdir pass gathers only the slot count and the name
 * bytes that a later indexing pass will need. The walk stops at the configured
 * entry cap and marks the directory truncated.
 *
 * @param[in,out] dir        Directory being measured.
 * @param[out]    entries    Receives the number of entries found.
 * @param[out]    slots      Receives the number of slots, including the terminator.
 * @param[out]    name_bytes Receives the bytes the names need.
 * @return 0 on success, or a negative errno from the source directory.
 */
static int lazy_dir_count(struct proxy_lazy_dir *dir, uint32_t *entries, uint32_t *slots,
	uint32_t *name_bytes)
{
	char path[VFS_PATH_MAX];
	struct vfs_dirent *entry;
	struct vfs_dir *handle;
	uint32_t found = 0U;
	uint32_t found_slots = 0U;
	uint64_t found_names = 0U;
	int error = 0;
	int result;

	*entries = 0U;
	*slots = 0U;
	*name_bytes = 0U;
	result = lazy_dir_path((uint32_t)(dir - lazy.dirs), path, sizeof(path));
	if (result != 0) {
		return result;
	}
	handle = vfs_opendir(path, &error);
	if (handle == NULL) {
		return error != 0 ? error : -EIO;
	}
	entry = lazy_alloc(sizeof(*entry));
	if (entry == NULL) {
		vfs_closedir(handle);
		return -ENOMEM;
	}
	result = vfs_readdir(handle, entry);
	while (result == 0 && found < CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES) {
		size_t name_length = strlen(entry->name) + 1U;

		found++;
		found_slots += lazy_name_entries(entry->name);
		found_names += name_length;
		result = vfs_readdir(handle, entry);
	}
	if (result == 0) {
		dir->truncated = true;
		lazy.truncated = true;
	} else if (result != -ENOENT) {
		vfs_closedir(handle);
		lazy_free(entry);
		return result;
	}
	vfs_closedir(handle);
	lazy_free(entry);
	found_slots += 1U;
	*entries = found;
	*slots = found_slots;
	*name_bytes = found_names > UINT32_MAX ? UINT32_MAX : (uint32_t)found_names;
	return 0;
}

/**
 * @brief Reserve the directory extent of a view directory.
 *
 * @details The extent is sized from the actual entry count of the source
 * directory, so a directory with more than one cluster of entries is rendered
 * correctly. A source directory that cannot be opened still gets one cluster,
 * so the view stays readable; its folder simply renders as empty.
 *
 * @param[in] identifier Directory identifier.
 * @return 0 on success, or -ENOSPC or -ENOMEM.
 */
static int lazy_dir_reserve_extent(uint32_t identifier)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[identifier];
	struct proxy_lazy_partition *part;
	uint32_t entries = 0U;
	uint32_t slots = 1U;
	uint32_t names = 0U;
	uint32_t clusters;
	int result;

	if (dir->is_root) {
		dir->extent_ready = true;
		return 0;
	}
	if (!dir->extent_ready) {
		part = &lazy.partitions[dir->partition];
		if (part->selected && part->mounted) {
			result = lazy_dir_count(dir, &entries, &slots, &names);
			if (result != 0) {
				pr_warn("lazy proxy: cannot index %s: %d\n",
					lazy_dir_name(identifier), result);
				dir->source_error = result;
				lazy.last_error = result;
				entries = 0U;
				slots = 1U;
				names = 0U;
			}
		} else {
			dir->source_error = -ENODEV;
		}
		dir->planned_entries = entries;
		dir->planned_slots = slots;
		dir->planned_name_bytes = names;
		clusters = (slots + PROXY_LAZY_SLOTS_PER_CLUSTER - 1U) /
			PROXY_LAZY_SLOTS_PER_CLUSTER;
		if (clusters == 0U) {
			clusters = 1U;
		}
		/* Leave a little room so the host can create entries without a directory extension. */
		clusters += CONFIG_STORAGE_PROXY_LAZY_DIR_SLACK_CLUSTERS;
		result = lazy_alloc_clusters(dir->partition, clusters, &dir->first_cluster,
			&dir->clusters);
		if (result != 0) {
			lazy.last_error = result;
			return result;
		}
		dir->extension_cluster = dir->first_cluster + dir->clusters - 1U;
		dir->extent_ready = true;
	}
	if (!dir->range_ready) {
		result = lazy_range_add(dir->first_cluster, dir->clusters, identifier,
			PROXY_LAZY_RANGE_DIRECTORY, PROXY_LAZY_INVALID_SLOT, 0U);
		if (result != 0) {
			lazy.last_error = result;
			return result;
		}
		dir->range_ready = true;
	}
	return 0;
}

/* ---- LRU cache ---- */

/**
 * @brief Remove one directory from the LRU list.
 *
 * @param[in,out] dir Directory to remove.
 */
static void lazy_lru_remove(struct proxy_lazy_dir *dir)
{
	if (dir->lru_prev != PROXY_LAZY_INVALID_DIR) {
		lazy.dirs[dir->lru_prev].lru_next = dir->lru_next;
	} else if (lazy.lru_head == (uint32_t)(dir - lazy.dirs)) {
		lazy.lru_head = dir->lru_next;
	}
	if (dir->lru_next != PROXY_LAZY_INVALID_DIR) {
		lazy.dirs[dir->lru_next].lru_prev = dir->lru_prev;
	} else if (lazy.lru_tail == (uint32_t)(dir - lazy.dirs)) {
		lazy.lru_tail = dir->lru_prev;
	}
	dir->lru_prev = PROXY_LAZY_INVALID_DIR;
	dir->lru_next = PROXY_LAZY_INVALID_DIR;
}

/**
 * @brief Mark a cached directory as the most recently used one.
 *
 * @param[in] identifier Directory identifier.
 */
static void lazy_lru_touch(uint32_t identifier)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[identifier];

	if (lazy.lru_tail == identifier) {
		return;
	}
	lazy_lru_remove(dir);
	dir->lru_prev = lazy.lru_tail;
	dir->lru_next = PROXY_LAZY_INVALID_DIR;
	if (lazy.lru_tail != PROXY_LAZY_INVALID_DIR) {
		lazy.dirs[lazy.lru_tail].lru_next = identifier;
	} else {
		lazy.lru_head = identifier;
	}
	lazy.lru_tail = identifier;
}

/**
 * @brief Free the entry cache of a directory and drop it from the LRU.
 *
 * @details The header, its extents and its child-directory list stay, so a
 * later reload reproduces the same slots and first clusters.
 *
 * @param[in] identifier Directory identifier.
 */
static void lazy_cache_evict(uint32_t identifier)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[identifier];

	if (!dir->cache_ready) {
		return;
	}
	if (dir->sizes_dirty) {
		for (uint32_t index = 0U; index < dir->entry_count; index++) {
			const struct proxy_lazy_dirent *item = &dir->entries[index];

			if (!item->directory && !item->deleted && item->clusters != 0U) {
				(void)lazy_truncate_entry(identifier, index, item->size);
			}
		}
		dir->sizes_dirty = false;
	}
	lazy_lru_remove(dir);
	if (lazy.cache_entries >= dir->entry_count) {
		lazy.cache_entries -= dir->entry_count;
	} else {
		lazy.cache_entries = 0U;
	}
	if (lazy.cache_bytes >= dir->cache_bytes) {
		lazy.cache_bytes -= dir->cache_bytes;
	} else {
		lazy.cache_bytes = 0U;
	}
	lazy_free(dir->entries);
	lazy_free(dir->names);
	dir->entries = NULL;
	dir->names = NULL;
	dir->entry_count = 0U;
	dir->entry_capacity = 0U;
	dir->name_capacity = 0U;
	dir->name_used = 0U;
	dir->cache_bytes = 0U;
	dir->cache_ready = false;
	lazy.evictions++;
}

/**
 * @brief Evict cached directories until a new cache fits.
 *
 * @param[in] bytes   Bytes the new cache needs.
 * @param[in] entries Entries the new cache needs.
 * @return 0 when room was made, or -ENOSPC when even an empty cache is too large.
 */
static int lazy_cache_make_room(uint32_t bytes, uint32_t entries)
{
	while (lazy.lru_head != PROXY_LAZY_INVALID_DIR &&
	       (lazy.cache_bytes + bytes > CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES ||
		lazy.cache_entries + entries > CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES)) {
		lazy_cache_evict(lazy.lru_head);
	}
	if (lazy.cache_bytes + bytes > CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES ||
	    lazy.cache_entries + entries > CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES) {
		return -ENOSPC;
	}
	return 0;
}

/* ---- overlay, pending data and FAT log ---- */

/**
 * @brief Store an overlay name in its arena.
 *
 * @param[in] name NUL-terminated name.
 * @return Offset in the arena, or UINT32_MAX when it is full.
 */
static uint32_t lazy_overlay_name_store(const char *name)
{
	size_t length = strlen(name) + 1U;
	uint32_t offset;

	if (lazy.overlay_names == NULL ||
	    length > CONFIG_STORAGE_PROXY_LAZY_OVERLAY_NAME_BYTES - lazy.overlay_name_used) {
		return UINT32_MAX;
	}
	offset = lazy.overlay_name_used;
	memcpy(lazy.overlay_names + offset, name, length);
	lazy.overlay_name_used += (uint32_t)length;
	return offset;
}

/**
 * @brief Find the overlay record of one name in a directory.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] name   Entry name.
 * @return The record, or NULL.
 */
static struct proxy_lazy_overlay *lazy_overlay_find_name(uint32_t dir_id, const char *name)
{
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		struct proxy_lazy_overlay *record = &lazy.overlay[index];

		if (record->used && record->dir_id == dir_id &&
		    strcasecmp(lazy.overlay_names + record->name_offset, name) == 0) {
			return record;
		}
	}
	return NULL;
}

/**
 * @brief Count the live overlay records of one directory.
 *
 * @param[in] dir_id Directory identifier.
 * @return The record count.
 */
static uint32_t lazy_overlay_dir_count(uint32_t dir_id)
{
	uint32_t count = 0U;

	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		if (lazy.overlay[index].used && lazy.overlay[index].dir_id == dir_id) {
			count++;
		}
	}
	return count;
}

/**
 * @brief Record a host-created or host-changed entry.
 *
 * @param[in] dir_id        Directory identifier.
 * @param[in] name          Entry name.
 * @param[in] slot          Absolute first slot of the entry group.
 * @param[in] first_cluster First cluster of the host extent, or zero.
 * @param[in] clusters      Number of clusters of the host extent.
 * @param[in] size          File size or directory data length.
 * @param[in] directory     True for a directory.
 * @param[in] fat_chain     True when the host extent is a FAT chain.
 * @return 0 on success, -ENOSPC when the overlay is full, or -ENOMEM.
 */
static int lazy_overlay_put(uint32_t dir_id, const char *name, uint32_t slot,
	uint32_t first_cluster, uint32_t clusters, uint64_t size, bool directory, bool fat_chain)
{
	struct proxy_lazy_overlay *record = lazy_overlay_find_name(dir_id, name);
	uint32_t name_offset;

	if (record == NULL) {
		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
			if (!lazy.overlay[index].used) {
				record = &lazy.overlay[index];
				break;
			}
		}
	}
	if (record == NULL) {
		return -ENOSPC;
	}
	if (!record->used) {
		name_offset = lazy_overlay_name_store(name);
		if (name_offset == UINT32_MAX) {
			return -ENOSPC;
		}
		memset(record, 0, sizeof(*record));
		record->used = true;
		record->name_offset = name_offset;
	} else if (strcmp(lazy.overlay_names + record->name_offset, name) != 0) {
		name_offset = lazy_overlay_name_store(name);
		if (name_offset == UINT32_MAX) {
			return -ENOSPC;
		}
		record->name_offset = name_offset;
	}
	record->dir_id = dir_id;
	record->slot = slot;
	record->first_cluster = first_cluster;
	record->clusters = clusters;
	record->size = size;
	record->entries = (uint16_t)lazy_name_entries(name);
	record->directory = directory;
	record->fat_chain = fat_chain;
	record->deleted = false;
	record->applied = false;
	lazy.overlay_count = 0U;
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		if (lazy.overlay[index].used) {
			lazy.overlay_count++;
		}
	}
	return 0;
}

/**
 * @brief Record that the host deleted an entry, or refresh such a record.
 *
 * @param[in] dir_id    Directory identifier.
 * @param[in] name      Entry name.
 * @param[in] slot      Absolute first slot of the deleted group.
 * @param[in] directory True when the object was a directory.
 * @param[in] entries   Slots the group occupied.
 * @return 0 on success, or -ENOSPC when the overlay is full.
 */
static int lazy_overlay_tombstone(uint32_t dir_id, const char *name, uint32_t slot,
	bool directory, uint16_t entries)
{
	struct proxy_lazy_overlay *record = lazy_overlay_find_name(dir_id, name);
	uint32_t name_offset;

	if (record == NULL) {
		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
			if (!lazy.overlay[index].used) {
				record = &lazy.overlay[index];
				break;
			}
		}
	}
	if (record == NULL) {
		return -ENOSPC;
	}
	if (!record->used) {
		name_offset = lazy_overlay_name_store(name);
		if (name_offset == UINT32_MAX) {
			return -ENOSPC;
		}
		memset(record, 0, sizeof(*record));
		record->used = true;
		record->name_offset = name_offset;
		lazy.overlay_count++;
	}
	record->dir_id = dir_id;
	record->slot = slot;
	record->first_cluster = 0U;
	record->clusters = 0U;
	record->size = 0U;
	record->entries = entries != 0U ? entries : (uint16_t)lazy_name_entries(name);
	record->directory = directory;
	record->fat_chain = false;
	record->deleted = true;
	record->applied = false;
	return 0;
}

/**
 * @brief Drop every overlay record of one directory.
 *
 * @details A host-created directory may reuse the name of one the host deleted.
 * Its old children no longer exist, so their records must not leak into the new
 * directory.
 *
 * @param[in] dir_id Directory identifier.
 */
static void lazy_overlay_clear_dir(uint32_t dir_id)
{
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		if (lazy.overlay[index].used && lazy.overlay[index].dir_id == dir_id) {
			lazy.overlay[index].used = false;
		}
	}
	lazy.overlay_count = 0U;
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		if (lazy.overlay[index].used) {
			lazy.overlay_count++;
		}
	}
}

/**
 * @brief Look up the next cluster of a host-written FAT chain.
 *
 * @param[in] cluster Cluster number.
 * @return The next cluster, or zero when the host did not write that entry.
 */
static uint32_t lazy_fat_next(uint32_t cluster)
{
	for (uint32_t index = 0U; index < lazy.fat_count; index++) {
		if (lazy.fat_log[index].used && lazy.fat_log[index].cluster == cluster) {
			return lazy.fat_log[index].next;
		}
	}
	return 0U;
}

/**
 * @brief Remember one FAT entry the host wrote.
 *
 * @param[in] cluster Cluster number.
 * @param[in] next    Next cluster value.
 * @return 0 on success, -ENOSPC when the log is full, or -ENOMEM.
 */
static int lazy_fat_set(uint32_t cluster, uint32_t next)
{
	for (uint32_t index = 0U; index < lazy.fat_count; index++) {
		if (lazy.fat_log[index].used && lazy.fat_log[index].cluster == cluster) {
			lazy.fat_log[index].next = next;
			return 0;
		}
	}
	if (next == 0U) {
		/* A cleared entry only matters for a cluster the log already knows. */
		return 0;
	}
	if (lazy.fat_count >= CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES) {
		return -ENOSPC;
	}
	lazy.fat_log[lazy.fat_count].cluster = cluster;
	lazy.fat_log[lazy.fat_count].next = next;
	lazy.fat_log[lazy.fat_count].used = true;
	lazy.fat_count++;
	return 0;
}

/**
 * @brief Parse one FAT sector the host wrote.
 *
 * @param[in] fat_sector Sector offset inside the FAT.
 * @param[in] data       Sector contents.
 * @return 0 on success, or -ENOSPC when the FAT log is full.
 */
static int lazy_fat_write(uint32_t fat_sector, const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	uint32_t first = fat_sector * (PROXY_LAZY_SECTOR_SIZE / 4U);
	int result = 0;

	for (uint32_t index = 0U; index < PROXY_LAZY_SECTOR_SIZE / 4U; index++) {
		uint32_t value = (uint32_t)data[index * 4U] |
			((uint32_t)data[index * 4U + 1U] << 8) |
			((uint32_t)data[index * 4U + 2U] << 16) |
			((uint32_t)data[index * 4U + 3U] << 24);

		if (value == 0U && lazy_fat_next(first + index) == 0U) {
			continue;
		}
		result = lazy_fat_set(first + index, value);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

/**
 * @brief Store one host sector until its directory entry arrives.
 *
 * @param[in] cluster View cluster.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, or -ENOSPC when the table is full.
 */
static int lazy_pending_store(uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		struct proxy_lazy_pending_sector *pending = &lazy.pending[index];

		if (pending->cluster == cluster && pending->sector == sector) {
			memcpy(pending->data, data, PROXY_LAZY_SECTOR_SIZE);
			pending->used = true;
			return 0;
		}
	}
	if (lazy.pending_count >= CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS) {
		return -ENOSPC;
	}
	{
		struct proxy_lazy_pending_sector *pending = &lazy.pending[lazy.pending_count++];

		pending->cluster = cluster;
		pending->sector = sector;
		pending->used = true;
		memcpy(pending->data, data, PROXY_LAZY_SECTOR_SIZE);
	}
	return 0;
}

/**
 * @brief Forget every pending sector of a cluster the host freed.
 *
 * @param[in] cluster Cluster that became free.
 */
static void lazy_pending_drop_cluster(uint32_t cluster)
{
	for (uint32_t index = 0U; index < lazy.pending_count; index++) {
		if (lazy.pending[index].cluster == cluster) {
			lazy.pending[index].used = false;
		}
	}
}

/**
 * @brief Forget the extent of a deleted entry whose clusters the host reused.
 *
 * @details A deleted entry keeps its extent so a host that clears the old group
 * before writing the renamed one is still matched. Once the bitmap frees the
 * clusters, though, a new object may receive them, and matching the old entry
 * to the new object would be wrong.
 *
 * @param[in] cluster Cluster that became free.
 */
static void lazy_forget_freed_cluster(uint32_t cluster)
{
	for (uint32_t dir = 0U; dir < lazy.dir_count; dir++) {
		struct proxy_lazy_dir *header = &lazy.dirs[dir];

		if (!header->cache_ready || header->entries == NULL) {
			continue;
		}
		for (uint32_t index = 0U; index < header->entry_count; index++) {
			struct proxy_lazy_dirent *item = &header->entries[index];

			if (item->deleted && item->clusters != 0U &&
			    cluster >= item->first_cluster &&
			    cluster < item->first_cluster + item->clusters) {
				item->first_cluster = 0U;
				item->clusters = 0U;
				header->ranges_dirty = true;
			}
		}
	}
}

/* ---- indexing a directory ---- */

/**
 * @brief Compare two cached entries by slot.
 *
 * @param[in] left  First element, an index into @c lazy_sort_entries.
 * @param[in] right Second element, an index into @c lazy_sort_entries.
 * @return Negative, zero or positive as @p left sorts before, with or after @p right.
 */
static int lazy_slot_compare(const void *left, const void *right)
{
	uint32_t left_index = *(const uint32_t *)left;
	uint32_t right_index = *(const uint32_t *)right;
	uint32_t left_slot = lazy_sort_entries[left_index].slot;
	uint32_t right_slot = lazy_sort_entries[right_index].slot;

	return left_slot < right_slot ? -1 : left_slot > right_slot ? 1 : 0;
}

/**
 * @brief Find an existing child header of a parent by name.
 *
 * @param[in] parent Parent directory.
 * @param[in] name   Child name.
 * @return The child identifier, or PROXY_LAZY_INVALID_DIR.
 */
static uint32_t lazy_dir_find_child(const struct proxy_lazy_dir *parent, const char *name)
{
	for (uint32_t index = 0U; index < parent->subdir_count; index++) {
		uint32_t identifier = parent->subdirs[index];

		if (strcasecmp(lazy_dir_name(identifier), name) == 0) {
			return identifier;
		}
	}
	return PROXY_LAZY_INVALID_DIR;
}

/**
 * @brief Find or create the header of one child directory.
 *
 * @details Child headers are looked up by name, so a source directory that is
 * reordered between loads still maps to the extent it was given the first time.
 * A host-created directory already has its header and host extent.
 *
 * @param[in,out] parent Parent directory.
 * @param[in]     name   Child name.
 * @param[out]    child  Receives the child identifier.
 * @return 0 on success, or a negative errno.
 */
static int lazy_dir_child(struct proxy_lazy_dir *parent, const char *name, uint32_t *child)
{
	uint32_t identifier = lazy_dir_find_child(parent, name);
	int result;

	if (identifier == PROXY_LAZY_INVALID_DIR) {
		result = lazy_subdir_reserve(parent);
		if (result != 0) {
			return result;
		}
		identifier = lazy_dir_alloc(name);
		if (identifier == UINT32_MAX) {
			return -ENOSPC;
		}
		lazy.dirs[identifier].parent = (uint32_t)(parent - lazy.dirs);
		lazy.dirs[identifier].partition = parent->partition;
		result = lazy_subdir_append(parent, identifier);
		if (result != 0) {
			return result;
		}
	}
	/* An earlier load may have run out of reservation before the extent existed. */
	result = lazy_dir_reserve_extent(identifier);
	if (result != 0) {
		return result;
	}
	*child = identifier;
	return 0;
}

/**
 * @brief Copy a name into a directory's name arena, growing it when needed.
 *
 * @param[in,out] dir    Directory receiving the name.
 * @param[in]     name   NUL-terminated name.
 * @param[out]    offset Receives the offset of the copy.
 * @return 0 on success, or -ENOSPC when the arena cannot grow.
 */
static int lazy_dir_name_copy(struct proxy_lazy_dir *dir, const char *name, uint32_t *offset)
{
	size_t length = strlen(name) + 1U;
	uint32_t capacity;

	if (dir->name_used + length <= dir->name_capacity) {
		*offset = dir->name_used;
		memcpy(dir->names + dir->name_used, name, length);
		dir->name_used += (uint32_t)length;
		return 0;
	}
	capacity = dir->name_capacity == 0U ? 512U : dir->name_capacity * 2U;
	while (capacity < dir->name_used + length) {
		if (capacity > UINT32_MAX / 2U) {
			return -ENOSPC;
		}
		capacity *= 2U;
	}
	if (capacity > CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES) {
		return -ENOSPC;
	}
	{
		char *names = lazy_alloc(capacity);

		if (names == NULL) {
			return -ENOMEM;
		}
		if (dir->names != NULL) {
			memcpy(names, dir->names, dir->name_used);
			lazy_free(dir->names);
		}
		dir->names = names;
		if (dir->cache_ready) {
			uint32_t growth = capacity - dir->name_capacity;

			lazy.cache_bytes += growth;
			dir->cache_bytes += growth;
		}
		dir->name_capacity = capacity;
	}
	*offset = dir->name_used;
	memcpy(dir->names + dir->name_used, name, length);
	dir->name_used += (uint32_t)length;
	return 0;
}

/**
 * @brief Make room for one more cached entry in a directory.
 *
 * @param[in,out] dir Directory that needs space.
 * @return 0 on success, -ENOSPC when the global cache is full, or -ENOMEM.
 */
static int lazy_dir_reserve_entry(struct proxy_lazy_dir *dir)
{
	uint32_t capacity;
	int result;

	if (dir->entry_count < dir->entry_capacity) {
		return 0;
	}
	capacity = dir->entry_capacity == 0U ? 64U : dir->entry_capacity * 2U;
	if (capacity > CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES) {
		capacity = CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES;
	}
	if (capacity <= dir->entry_capacity) {
		return -ENOSPC;
	}
	result = lazy_cache_make_room((capacity - dir->entry_capacity) *
		(uint32_t)sizeof(struct proxy_lazy_dirent), capacity - dir->entry_capacity);
	if (result != 0) {
		return result;
	}
	{
		struct proxy_lazy_dirent *entries = lazy_calloc(capacity, sizeof(*entries));

		if (entries == NULL) {
			return -ENOMEM;
		}
		if (dir->entries != NULL) {
			memcpy(entries, dir->entries,
				(size_t)dir->entry_count * sizeof(*entries));
			lazy_free(dir->entries);
		}
		dir->entries = entries;
		if (dir->cache_ready) {
			uint32_t growth = (capacity - dir->entry_capacity) *
				(uint32_t)sizeof(struct proxy_lazy_dirent);

			lazy.cache_bytes += growth;
			dir->cache_bytes += growth;
		}
		dir->entry_capacity = capacity;
	}
	return 0;
}

/**
 * @brief Keep a cached directory sorted by slot.
 *
 * @param[in,out] dir Directory to sort.
 */
static void lazy_dir_sort(struct proxy_lazy_dir *dir)
{
	uint32_t *order;

	if (dir->entry_count < 2U) {
		return;
	}
	order = lazy_alloc((size_t)dir->entry_count * sizeof(*order));
	if (order == NULL) {
		/* A failed sort leaves the array in insertion order, which is still usable. */
		return;
	}
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		order[index] = index;
	}
	lazy_sort_entries = dir->entries;
	qsort(order, dir->entry_count, sizeof(*order), lazy_slot_compare);
	lazy_sort_entries = NULL;
	{
		struct proxy_lazy_dirent *sorted = lazy_alloc((size_t)dir->entry_count *
			sizeof(*sorted));

		if (sorted == NULL) {
			lazy_free(order);
			return;
		}
		for (uint32_t index = 0U; index < dir->entry_count; index++) {
			sorted[index] = dir->entries[order[index]];
		}
		memcpy(dir->entries, sorted, (size_t)dir->entry_count * sizeof(*sorted));
		lazy_free(sorted);
	}
	lazy_free(order);
}

/**
 * @brief Find a cached entry by name.
 *
 * @param[in] dir            Directory to search.
 * @param[in] name           Name to find.
 * @param[in] include_deleted Whether a deleted entry may match.
 * @return The entry index, or UINT32_MAX.
 */
static uint32_t lazy_dir_find_name(const struct proxy_lazy_dir *dir, const char *name,
	bool include_deleted)
{
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if ((!item->deleted || include_deleted) &&
		    strcasecmp(dir->names + item->name_offset, name) == 0) {
			return index;
		}
	}
	return UINT32_MAX;
}

/**
 * @brief Find a cached entry by one cluster of its extent.
 *
 * @details Directories use the same lookup as files, which is what turns a
 * clear-and-rewrite of a directory group at another slot into a directory
 * rename.
 *
 * @param[in] dir     Directory to search.
 * @param[in] cluster Cluster to find.
 * @param[in] include_deleted Whether a deleted entry may match.
 * @return The entry index, or UINT32_MAX.
 */
static uint32_t lazy_dir_find_cluster(const struct proxy_lazy_dir *dir, uint32_t cluster,
	bool include_deleted)
{
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if ((!item->deleted || include_deleted) &&
		    item->clusters != 0U && cluster >= item->first_cluster &&
		    cluster < item->first_cluster + item->clusters) {
			return index;
		}
	}
	return UINT32_MAX;
}

/**
 * @brief Remove a deleted entry whose slots collide with a new group.
 *
 * @param[in,out] dir      Directory to change.
 * @param[in]     slot     First slot of the new group.
 * @param[in]     entries  Number of slots of the new group.
 */
static void lazy_dir_clear_deleted(struct proxy_lazy_dir *dir, uint32_t slot, uint32_t entries)
{
	uint32_t kept = 0U;

	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		struct proxy_lazy_dirent *item = &dir->entries[index];

		if (item->deleted && item->slot < slot + entries &&
		    slot < item->slot + item->entries) {
			if (lazy.cache_entries != 0U) {
				lazy.cache_entries--;
			}
			continue;
		}
		if (kept != index) {
			dir->entries[kept] = *item;
		}
		kept++;
	}
	dir->entry_count = kept;
}

/**
 * @brief Insert a new cached entry at the host's slot.
 *
 * @param[in,out] dir          Directory receiving the entry.
 * @param[in]     name         Entry name.
 * @param[in]     directory    True for a directory.
 * @param[in]     slot         Absolute first slot.
 * @param[in]     first_cluster First cluster of the extent, or zero.
 * @param[in]     clusters     Number of clusters of the extent.
 * @param[in]     size         File size or directory data length.
 * @param[in]     host_owned   True when the extent comes from the host.
 * @param[in]     fat_chain    True when the host extent is a FAT chain.
 * @param[out]    index        Receives the entry index.
 * @return 0 on success, or a negative errno.
 */
static int lazy_dir_insert(struct proxy_lazy_dir *dir, const char *name, bool directory,
	uint32_t slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool host_owned, bool fat_chain, uint32_t *index)
{
	struct proxy_lazy_dirent *item;
	uint32_t name_offset;
	uint32_t position;
	int result = lazy_dir_reserve_entry(dir);

	if (result != 0) {
		return result;
	}
	result = lazy_dir_name_copy(dir, name, &name_offset);
	if (result != 0) {
		return result;
	}
	lazy_dir_clear_deleted(dir, slot, lazy_name_entries(name));
	position = dir->entry_count;
	for (uint32_t cursor = 0U; cursor < dir->entry_count; cursor++) {
		if (dir->entries[cursor].slot > slot) {
			position = cursor;
			break;
		}
	}
	memmove(&dir->entries[position + 1U], &dir->entries[position],
		(size_t)(dir->entry_count - position) * sizeof(*dir->entries));
	item = &dir->entries[position];
	memset(item, 0, sizeof(*item));
	item->name_offset = name_offset;
	item->slot = slot;
	item->entries = (uint16_t)lazy_name_entries(name);
	item->directory = directory;
	item->first_cluster = first_cluster;
	item->clusters = clusters;
	item->size = size;
	item->host_owned = host_owned;
	item->fat_chain = fat_chain;
	dir->entry_count++;
	lazy.cache_entries++;
	*index = position;
	return 0;
}

/**
 * @brief Add the ranges of one host extent, walking its FAT chain when needed.
 *
 * @param[in] dir_id   Directory that owns the file.
 * @param[in] slot     Absolute slot of the entry.
 * @param[in] first    First cluster of the extent.
 * @param[in] expected Number of clusters the size asks for.
 * @param[in] fat_chain True when the host used a FAT chain.
 * @return 0 on success, or a negative errno.
 */
static int lazy_add_file_ranges(uint32_t dir_id, uint32_t slot, uint32_t first,
	uint32_t expected, bool fat_chain)
{
	uint32_t cluster = first;
	uint32_t offset = 0U;
	int result;

	if (expected == 0U) {
		return 0;
	}
	if (first < PROXY_LAZY_ROOT_CLUSTER_MIN ||
	    (uint64_t)first + expected > (uint64_t)lazy.total_clusters + 2U) {
		return -EIO;
	}
	if (!fat_chain) {
		return lazy_range_add(first, expected, dir_id, PROXY_LAZY_RANGE_DATA, slot, 0U);
	}
	while (offset < expected) {
		uint32_t run = 1U;

		while (offset + run < expected) {
			uint32_t next = lazy_fat_next(cluster + run - 1U);

			if (next != cluster + run) {
				break;
			}
			run++;
		}
		if (cluster < PROXY_LAZY_ROOT_CLUSTER_MIN ||
		    (uint64_t)cluster + run > (uint64_t)lazy.total_clusters + 2U) {
			return -EIO;
		}
		result = lazy_range_add(cluster, run, dir_id, PROXY_LAZY_RANGE_DATA, slot, offset);
		if (result != 0) {
			return result;
		}
		offset += run;
		if (offset < expected) {
			cluster = lazy_fat_next(cluster + run - 1U);
			if (cluster < PROXY_LAZY_ROOT_CLUSTER_MIN ||
			    cluster >= lazy.total_clusters + 2U) {
				return -EIO;
			}
		}
	}
	return 0;
}

/**
 * @brief Rebuild every data range of a directory from its cached entries.
 *
 * @param[in] dir_id Directory identifier.
 * @return 0 on success, or a negative errno.
 */
static int lazy_rebuild_data_ranges(uint32_t dir_id)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	int result = 0;

	lazy_range_remove_owner(dir_id, PROXY_LAZY_RANGE_DATA);
	for (uint32_t index = 0U; index < dir->entry_count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if (item->directory || item->deleted || item->clusters == 0U) {
			continue;
		}
		result = lazy_add_file_ranges(dir_id, item->slot, item->first_cluster,
			item->clusters, item->fat_chain);
		if (result != 0) {
			lazy.last_error = result;
			return result;
		}
	}
	dir->ranges_dirty = false;
	return 0;
}

/**
 * @brief Return the next slot that no entry and no overlay record occupies.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] dir    Directory being built.
 * @param[in] count  Number of entries added so far.
 * @param[in] group  Slots the candidate entry group occupies.
 * @param[in] slot   Candidate slot.
 * @return A free absolute slot.
 */
static uint32_t lazy_free_slot(uint32_t dir_id, const struct proxy_lazy_dir *dir,
	uint32_t count, uint32_t group, uint32_t slot)
{
	for (;;) {
		bool blocked = false;

		for (uint32_t index = 0U; index < count; index++) {
			if (slot < dir->entries[index].slot + dir->entries[index].entries &&
			    dir->entries[index].slot < slot + group) {
				blocked = true;
				break;
			}
		}
		for (uint32_t index = 0U; !blocked &&
		     index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
			const struct proxy_lazy_overlay *record = &lazy.overlay[index];

			if (record->used && record->dir_id == dir_id) {
				if (slot < record->slot + record->entries &&
				    record->slot < slot + group) {
					blocked = true;
				}
			}
		}
		if (!blocked) {
			return slot;
		}
		slot += 2U;
	}
}

/**
 * @brief Populate the entry cache of a directory.
 *
 * @details The extent was sized by the count pass. The load pass reads the
 * source directory, skips entries an overlay tombstone hides, adopts the slot
 * and extent of every entry the host changed, appends host-created entries and
 * finally assigns contiguous data extents to the remaining source files. The
 * result is sorted by slot so a sector renders in one binary search.
 *
 * @param[in] identifier Directory identifier.
 * @return 0 on success, or a negative errno.
 */
static int lazy_dir_load(uint32_t identifier)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[identifier];
	struct vfs_dirent *entry = NULL;
	struct vfs_dir *handle = NULL;
	char path[VFS_PATH_MAX];
	uint32_t maximum;
	uint32_t capacity;
	uint32_t names_capacity;
	uint32_t cache_bytes;
	uint32_t slot_cursor = 0U;
	uint32_t count = 0U;
	uint32_t needed = 0U;
	uint32_t cursor;
	int error = 0;
	int result;

	if (dir->cache_ready) {
		lazy_lru_touch(identifier);
		return 0;
	}
	if (dir->is_root) {
		dir->cache_ready = true;
		return 0;
	}
	result = lazy_dir_reserve_extent(identifier);
	if (result != 0) {
		return result;
	}
	if (dir->source_error != 0) {
		dir->cache_ready = true;
		dir->entry_count = 0U;
		return 0;
	}
	maximum = dir->planned_entries;
	if (maximum > CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES) {
		maximum = CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES;
	}
	capacity = maximum + lazy_overlay_dir_count(identifier) +
		CONFIG_STORAGE_PROXY_LAZY_DIR_ENTRY_SLACK;
	if (capacity > CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES) {
		capacity = CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES;
	}
	if (capacity == 0U) {
		dir->data_ready = true;
		dir->data_range_ready = true;
		dir->entry_count = 0U;
		dir->cache_bytes = 0U;
		dir->cache_ready = true;
		lazy_lru_touch(identifier);
		return 0;
	}
	names_capacity = dir->planned_name_bytes;
	if (names_capacity < 512U) {
		names_capacity = 512U;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		const struct proxy_lazy_overlay *record = &lazy.overlay[index];

		if (record->used && record->dir_id == identifier) {
			names_capacity += (uint32_t)strlen(lazy.overlay_names +
				record->name_offset) + 1U;
		}
	}
	if (names_capacity > CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES) {
		names_capacity = CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES;
	}
	cache_bytes = capacity * (uint32_t)sizeof(struct proxy_lazy_dirent) + names_capacity;
	result = lazy_cache_make_room(cache_bytes, capacity);
	if (result != 0) {
		lazy.truncated = true;
		lazy.last_error = result;
		return result;
	}
	dir->entries = lazy_calloc(capacity, sizeof(*dir->entries));
	dir->names = lazy_alloc(names_capacity);
	if (dir->entries == NULL || dir->names == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	dir->entry_capacity = capacity;
	dir->name_capacity = names_capacity;
	dir->name_used = 0U;
	result = lazy_dir_path(identifier, path, sizeof(path));
	if (result != 0) {
		goto fail;
	}
	handle = vfs_opendir(path, &error);
	if (handle == NULL) {
		result = error != 0 ? error : -EIO;
		goto fail;
	}
	entry = lazy_alloc(sizeof(*entry));
	if (entry == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	result = vfs_readdir(handle, entry);
	while (result == 0 && count < capacity) {
		struct proxy_lazy_dirent *item;
		struct proxy_lazy_overlay *record = lazy_overlay_find_name(identifier,
			entry->name);
		uint32_t name_offset;

		if (record != NULL && record->deleted) {
			/* The host deleted it; a placeholder is added below. */
			result = vfs_readdir(handle, entry);
			continue;
		}
		if (lazy_dir_name_copy(dir, entry->name, &name_offset) != 0) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
		item = &dir->entries[count];
		memset(item, 0, sizeof(*item));
		item->name_offset = name_offset;
		item->entries = (uint16_t)lazy_name_entries(entry->name);
		item->directory = entry->st.dir;
		if (record != NULL) {
			item->slot = record->slot;
			item->first_cluster = record->first_cluster;
			item->clusters = record->clusters;
			item->size = record->size;
			item->directory = record->directory;
			item->host_owned = true;
			item->fat_chain = record->fat_chain;
		} else {
			item->slot = lazy_free_slot(identifier, dir, count,
				item->entries, slot_cursor);
			slot_cursor = item->slot + item->entries;
			if (slot_cursor < item->slot) {
				slot_cursor = item->entries;
			}
		}
		if (item->directory) {
			uint32_t child;

			result = lazy_dir_child(dir, entry->name, &child);
			if (result == -ENOSPC) {
				dir->truncated = true;
				lazy.truncated = true;
				result = 0;
				break;
			}
			if (result != 0) {
				goto fail;
			}
			if (!item->host_owned) {
				item->first_cluster = lazy.dirs[child].first_cluster;
				item->clusters = lazy.dirs[child].clusters;
			}
			item->fat_chain = lazy.dirs[child].chained;
		} else if (!item->host_owned) {
			uint64_t clusters = (entry->st.size + PROXY_LAZY_CLUSTER_SIZE - 1U) /
				PROXY_LAZY_CLUSTER_SIZE;

			if (clusters > UINT32_MAX) {
				result = -EFBIG;
				goto fail;
			}
			item->size = entry->st.size;
			item->clusters = (uint32_t)clusters;
		}
		count++;
		result = vfs_readdir(handle, entry);
	}
	if (result == 0) {
		dir->truncated = true;
		lazy.truncated = true;
	} else if (result != -ENOENT) {
		goto fail;
	}
	vfs_closedir(handle);
	handle = NULL;
	lazy_free(entry);
	entry = NULL;
	dir->entry_count = count;
	/* Entries only the overlay knows about: host-created objects. */
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		const struct proxy_lazy_overlay *record = &lazy.overlay[index];
		struct proxy_lazy_dirent *item;
		const char *name;
		uint32_t name_offset;

		if (!record->used || record->dir_id != identifier) {
			continue;
		}
		name = lazy.overlay_names + record->name_offset;
		if (!record->deleted &&
		    lazy_dir_find_name(dir, name, true) != UINT32_MAX) {
			continue;
		}
		if (lazy_dir_name_copy(dir, name, &name_offset) != 0) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
		item = &dir->entries[count];
		memset(item, 0, sizeof(*item));
		item->name_offset = name_offset;
		item->entries = (uint16_t)lazy_name_entries(name);
		item->slot = record->slot;
		item->directory = record->directory;
		item->deleted = record->deleted;
		if (!record->deleted) {
			item->first_cluster = record->first_cluster;
			item->clusters = record->clusters;
			item->size = record->size;
			item->host_owned = true;
			item->fat_chain = record->fat_chain;
		}
		count++;
		if (count >= capacity) {
			dir->truncated = true;
			lazy.truncated = true;
			break;
		}
	}
	lazy_dir_sort(dir);
	/* Clamp overlapping groups forward: the host must never see two owners of a slot. */
	{
		uint32_t used = 0U;

		for (uint32_t index = 0U; index < count; index++) {
			struct proxy_lazy_dirent *item = &dir->entries[index];

			if (index != 0U && item->slot < used) {
				item->slot = used;
				dir->truncated = true;
				lazy.truncated = true;
			}
			used = item->slot + item->entries;
		}
	}
	/* Hand out contiguous data clusters for the files the source owns. */
	for (uint32_t index = 0U; index < count; index++) {
		const struct proxy_lazy_dirent *item = &dir->entries[index];

		if (!item->directory && !item->deleted && !item->host_owned) {
			needed += item->clusters;
		}
	}
	if (!dir->data_ready || needed > dir->data_clusters) {
		result = lazy_alloc_clusters(dir->partition, needed, &dir->data_first_cluster,
			&dir->data_clusters);
		if (result != 0) {
			goto fail;
		}
		dir->data_ready = true;
	}
	cursor = dir->data_first_cluster;
	for (uint32_t index = 0U; index < count; index++) {
		struct proxy_lazy_dirent *item = &dir->entries[index];

		if (item->directory || item->deleted || item->host_owned || item->clusters == 0U) {
			continue;
		}
		if ((uint64_t)cursor + item->clusters >
		    (uint64_t)dir->data_first_cluster + dir->data_clusters) {
			result = -EIO;
			goto fail;
		}
		item->first_cluster = cursor;
		cursor += item->clusters;
	}
	dir->entry_count = count;
	dir->data_range_ready = true;
	result = lazy_rebuild_data_ranges(identifier);
	if (result != 0) {
		goto fail;
	}
	/* Account for the capacity the load actually ended up with. */
	cache_bytes = dir->entry_capacity * (uint32_t)sizeof(struct proxy_lazy_dirent) +
		dir->name_capacity;
	dir->cache_bytes = cache_bytes;
	lazy.cache_entries += count;
	lazy.cache_bytes += cache_bytes;
	dir->cache_ready = true;
	lazy_lru_touch(identifier);
	return 0;
fail:
	if (handle != NULL) {
		vfs_closedir(handle);
	}
	lazy_free(entry);
	lazy_free(dir->entries);
	lazy_free(dir->names);
	dir->entries = NULL;
	dir->names = NULL;
	dir->entry_count = 0U;
	dir->entry_capacity = 0U;
	dir->name_capacity = 0U;
	dir->name_used = 0U;
	pr_warn("lazy proxy: load %s failed: %d\n", lazy_dir_name(identifier), result);
	lazy.last_error = result;
	return result;
}

/* ---- exFAT entry rendering ---- */

/**
 * @brief Build one raw exFAT directory entry of an object.
 *
 * @details Entry zero is the stream entry, entry one is the stream extension
 * with length, hash, first cluster and both data lengths, and the remaining
 * entries are the name characters in 15-code-unit chunks.
 *
 * @param[in]  name          UTF-8 object name.
 * @param[in]  directory     True for a directory.
 * @param[in]  first_cluster First cluster of the object's extent.
 * @param[in]  clusters      Number of clusters of a directory extent.
 * @param[in]  size          File size in bytes.
 * @param[in]  fat_chain     True when the extent is a FAT chain.
 * @param[in]  position      Entry position inside the object's group.
 * @param[out] entry         Buffer receiving the 32-byte entry.
 */
static void lazy_entry_raw(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain, uint32_t position, uint8_t entry[32])
{
	uint16_t characters[VFS_NAME_MAX + 1];
	size_t length = lazy_utf16(name, characters, VFS_NAME_MAX);
	uint32_t entries = 2U + (uint32_t)((length + 14U) / 15U);
	uint64_t data_length = directory ? (uint64_t)clusters * PROXY_LAZY_CLUSTER_SIZE : size;

	memset(entry, 0, 32U);
	if (position == 0U) {
		entry[0] = 0x85U;
		entry[1] = (uint8_t)(entries - 1U);
		/* The host may write every object: no read-only attribute. */
		lazy_put_le16(entry + 4U, directory ? 0x10U : 0x20U);
	} else if (position == 1U) {
		entry[0] = 0xc0U;
		/* AllocationPossible, and NoFatChain unless the extent is a chain. */
		entry[1] = fat_chain ? 0x01U : 0x03U;
		entry[3] = (uint8_t)length;
		lazy_put_le16(entry + 4U, lazy_name_hash(characters, length));
		lazy_put_le64(entry + 8U, data_length);
		lazy_put_le32(entry + 20U, first_cluster);
		lazy_put_le64(entry + 24U, data_length);
	} else {
		size_t start = (size_t)(position - 2U) * 15U;

		entry[0] = 0xc1U;
		for (uint32_t character = 0U; character < 15U; character++) {
			size_t name_index = start + character;

			lazy_put_le16(entry + 2U + character * 2U,
				name_index < length ? characters[name_index] : 0U);
		}
	}
}

/**
 * @brief Compute the checksum of an object's whole entry group.
 *
 * @param[in] name          UTF-8 object name.
 * @param[in] directory     True for a directory.
 * @param[in] first_cluster First cluster of the object's extent.
 * @param[in] clusters      Number of clusters of a directory extent.
 * @param[in] size          File size in bytes.
 * @param[in] fat_chain     True when the extent is a FAT chain.
 * @return The entry-set checksum.
 */
static uint16_t lazy_entry_checksum(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain)
{
	uint8_t entry[32];
	uint32_t entries = lazy_name_entries(name);
	uint16_t checksum = 0U;

	for (uint32_t position = 0U; position < entries; position++) {
		lazy_entry_raw(name, directory, first_cluster, clusters, size, fat_chain,
			position, entry);
		for (uint32_t index = 0U; index < sizeof(entry); index++) {
			if (position == 0U && (index == 2U || index == 3U)) {
				continue;
			}
			checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
				(checksum >> 1) + entry[index]);
		}
	}
	return checksum;
}

/**
 * @brief Build one directory entry of an object, with its checksum filled in.
 *
 * @param[in]  name          UTF-8 object name.
 * @param[in]  directory     True for a directory.
 * @param[in]  first_cluster First cluster of the object's extent.
 * @param[in]  clusters      Number of clusters of a directory extent.
 * @param[in]  size          File size in bytes.
 * @param[in]  fat_chain     True when the extent is a FAT chain.
 * @param[in]  position      Entry position inside the object's group.
 * @param[out] entry         Buffer receiving the 32-byte entry.
 */
static void lazy_render_entry(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain, uint32_t position, uint8_t entry[32])
{
	lazy_entry_raw(name, directory, first_cluster, clusters, size, fat_chain, position,
		entry);
	if (position == 0U) {
		lazy_put_le16(entry + 2U, lazy_entry_checksum(name, directory, first_cluster,
			clusters, size, fat_chain));
	}
}

/**
 * @brief Render one of the three fixed system entries of the root directory.
 *
 * @param[in]  position System entry index.
 * @param[out] entry    Buffer receiving the 32-byte entry.
 */
static void lazy_root_system_entry(uint32_t position, uint8_t entry[32])
{
	/** @brief Volume label stored in the root directory. */
	static const uint16_t label[] = { 'L', 'A', 'Z', 'Y', 'V', 'I', 'E', 'W' };

	memset(entry, 0, 32U);
	if (position == 0U) {
		entry[0] = 0x83U;
		entry[1] = sizeof(label) / sizeof(label[0]);
		for (uint32_t index = 0U; index < sizeof(label) / sizeof(label[0]); index++) {
			lazy_put_le16(entry + 2U + index * 2U, label[index]);
		}
	} else if (position == 1U) {
		entry[0] = 0x81U;
		lazy_put_le32(entry + 20U, lazy.bitmap_cluster);
		lazy_put_le64(entry + 24U, lazy.bitmap_bytes);
	} else {
		entry[0] = 0x82U;
		lazy_put_le32(entry + 4U, lazy.upcase_checksum);
		lazy_put_le32(entry + 20U, lazy.upcase_cluster);
		lazy_put_le64(entry + 24U, lazy.upcase_bytes);
	}
}

/**
 * @brief Find the cached entry that occupies a slot.
 *
 * @details Entries are stored in readdir order and their slots increase
 * monotonically, so a binary search finds the group.
 *
 * @param[in] dir    Cached directory.
 * @param[in] wanted Absolute slot number.
 * @return The entry index, or UINT32_MAX.
 */
static uint32_t lazy_entry_by_slot(const struct proxy_lazy_dir *dir, uint32_t wanted)
{
	uint32_t low = 0U;
	uint32_t high = dir->entry_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const struct proxy_lazy_dirent *item = &dir->entries[middle];

		if (wanted < item->slot) {
			high = middle;
		} else if (wanted >= item->slot + item->entries) {
			low = middle + 1U;
		} else {
			return middle;
		}
	}
	return UINT32_MAX;
}

/**
 * @brief Render a root directory slot, reserving a partition folder if needed.
 *
 * @param[in]  slot  Absolute slot number.
 * @param[out] entry Buffer receiving the 32-byte entry, or zeros.
 * @return 0 on success, or a negative errno.
 */
static int lazy_render_root_slot(uint32_t slot, uint8_t entry[32])
{
	memset(entry, 0, 32U);
	if (slot < PROXY_LAZY_ROOT_SYSTEM_SLOTS) {
		lazy_root_system_entry(slot, entry);
		return 0;
	}
	for (uint32_t index = 0U; index < lazy.root_entry_count; index++) {
		const struct proxy_lazy_root_entry *root_entry = &lazy.root_entries[index];

		if (slot >= root_entry->slot && slot < root_entry->slot + root_entry->entries) {
			struct proxy_lazy_dir *dir = &lazy.dirs[root_entry->dir_id];
			int result = lazy_dir_reserve_extent(root_entry->dir_id);

			if (result != 0) {
				return result;
			}
			lazy_render_entry(lazy_dir_name(root_entry->dir_id), true, dir->first_cluster,
				dir->clusters, 0U, dir->chained, slot - root_entry->slot, entry);
			return 0;
		}
	}
	return 0;
}

/**
 * @brief Render one sector of a view directory.
 *
 * @param[in]  identifier Directory identifier.
 * @param[in]  offset     Byte offset of the sector inside the directory extent.
 * @param[out] sector     Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
static int lazy_render_directory(uint32_t identifier, uint64_t offset,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	uint32_t first_slot = (uint32_t)(offset / 32U);
	int result = 0;

	memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
	if (identifier == PROXY_LAZY_ROOT_ID) {
		for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
			result = lazy_render_root_slot(first_slot + slot,
				sector + slot * 32U);
			if (result != 0) {
				return result;
			}
		}
		return 0;
	}
	result = lazy_dir_load(identifier);
	if (result != 0) {
		return result;
	}
	{
		const struct proxy_lazy_dir *dir = &lazy.dirs[identifier];
		uint32_t last_slot = dir->entry_count != 0U ?
			dir->entries[dir->entry_count - 1U].slot : 0U;

		for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
			uint32_t absolute = first_slot + slot;
			uint32_t index = lazy_entry_by_slot(dir, absolute);

			if (index != UINT32_MAX) {
				const struct proxy_lazy_dirent *item = &dir->entries[index];

				lazy_render_entry(dir->names + item->name_offset, item->directory,
					item->first_cluster, item->clusters, item->size,
					item->fat_chain, absolute - item->slot,
					sector + slot * 32U);
				if (item->deleted) {
					/* exFAT marks a deleted entry by clearing its in-use bit. */
					sector[slot * 32U] &= 0x7fU;
				}
			} else if (last_slot > absolute) {
				/*
				 * A free slot before a live entry must not read as zero: zero is
				 * the end-of-directory marker. A deleted stream entry says "skip me".
				 */
				sector[slot * 32U] = 0x05U;
			}
		}
	}
	return 0;
}

/* ---- file data ---- */

/**
 * @brief Close the cached write descriptor, flushing the file.
 */
static void lazy_write_cache_drop(void)
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
static int lazy_read_file(uint32_t dir_id, uint32_t entry_index, uint64_t offset,
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

/* ---- applying host changes ---- */

/** @brief Carry out the deferred deletes; defined with the flush path below. */
static int lazy_apply_deletes_locked(void);

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
static int lazy_fill_to(int descriptor, uint64_t position)
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
static int lazy_write_bytes(uint32_t dir_id, uint32_t entry_index, uint64_t offset,
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
static int lazy_claim_pending_dir(uint32_t dir_id)
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
 * @brief Build the source path of a child of a directory.
 *
 * @param[in]  dir_id Directory identifier.
 * @param[in]  name   Child name.
 * @param[out] path   Buffer receiving the path.
 * @param[in]  size   Size of @p path.
 * @return 0 on success, or a negative errno.
 */
static int lazy_child_path(uint32_t dir_id, const char *name, char *path, size_t size)
{
	size_t length;
	int written;

	if (lazy_dir_path(dir_id, path, size) != 0) {
		return -ENAMETOOLONG;
	}
	length = strlen(path);
	written = snprintf(path + length, size - length, "/%s", name);
	if (written < 0 || (size_t)written >= size - length) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/**
 * @brief Create a source object the host created.
 *
 * @details A source name that still exists because an earlier delete was not
 * flushed yet is handled the way the eager engine does it: the deferred deletes
 * are carried out and the create is retried once.
 *
 * @param[in] dir_id    Parent directory.
 * @param[in] name      Entry name.
 * @param[in] directory True for a directory.
 * @return 0 on success, or a negative errno.
 */
static int lazy_source_create(uint32_t dir_id, const char *name, bool directory)
{
	char path[VFS_PATH_MAX];
	int result = lazy_child_path(dir_id, name, path, sizeof(path));

	for (uint32_t attempt = 0U; result == 0 && attempt < 2U; attempt++) {
		if (directory) {
			result = vfs_mkdir(path);
		} else {
			int descriptor = vfs_open(path,
				VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL);

			result = descriptor < 0 ? descriptor : vfs_close(descriptor);
		}
		if (result != -EEXIST || attempt != 0U) {
			break;
		}
		result = lazy_apply_deletes_locked();
	}
	return result;
}

/**
 * @brief Truncate the source file of a cached entry.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index.
 * @param[in] size        New size.
 * @return 0 on success, or a negative errno.
 */
static int lazy_truncate_entry(uint32_t dir_id, uint32_t entry_index, uint64_t size)
{
	const struct proxy_lazy_dirent *item = &lazy.dirs[dir_id].entries[entry_index];
	char path[VFS_PATH_MAX];
	int descriptor;
	int result;

	if (item->directory) {
		return 0;
	}
	result = lazy_entry_path(dir_id, item, path, sizeof(path));
	if (result != 0) {
		return result;
	}
	if (lazy.write_fd >= 0 && lazy.write_dir == dir_id &&
	    lazy.write_slot == item->slot) {
		lazy_write_cache_drop();
	}
	if (lazy.cached_fd >= 0 && lazy.cached_dir == dir_id &&
	    lazy.cached_slot == item->slot) {
		(void)vfs_close(lazy.cached_fd);
		lazy.cached_fd = -1;
		lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	}
	descriptor = vfs_open(path, VFS_O_WRITE);
	if (descriptor < 0) {
		return descriptor == -ENOENT ? 0 : descriptor;
	}
	result = vfs_ftruncate(descriptor, size);
	(void)vfs_close(descriptor);
	return result;
}

/**
 * @brief Send the ordinary entry fields of a host write to the source.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index.
 * @param[in] size        New file size or directory data length.
 * @return 0 on success, or a negative errno.
 */
static int lazy_entry_resize(uint32_t dir_id, uint32_t entry_index, uint64_t size)
{
	struct proxy_lazy_dirent *item = &lazy.dirs[dir_id].entries[entry_index];
	uint64_t old_size = item->size;

	item->size = size;
	if (!item->directory && old_size != size) {
		return lazy_truncate_entry(dir_id, entry_index, size);
	}
	return 0;
}

/**
 * @brief Mark the cached entry of a directory as a FAT chain.
 *
 * @details The parent entry advertises the child's extent; once the child is
 * fragmented, that entry must clear exFAT's NoFatChain flag so the host follows
 * the FAT instead of assuming contiguity.
 *
 * @param[in] dir_id Directory whose extent became a chain.
 */
static void lazy_dir_parent_chained(uint32_t dir_id)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dir *parent;
	uint32_t index;

	if (dir->parent >= lazy.dir_count) {
		return;
	}
	parent = &lazy.dirs[dir->parent];
	if (!parent->cache_ready || parent->entries == NULL) {
		return;
	}
	index = lazy_dir_find_name(parent, lazy_dir_name(dir_id), false);
	if (index != UINT32_MAX) {
		parent->entries[index].fat_chain = true;
	}
}

/**
 * @brief Try to grow a directory extent within the reservation.
 *
 * @details exFAT directories are contiguous unless their chain is fragmented.
 * Clusters that follow the extent and belong to no other object can simply
 * join it; anything else is refused rather than misrepresented.
 *
 * @param[in] dir_id  Directory identifier.
 * @param[in] wanted  Clusters the directory data length asks for.
 * @return 0 on success, or -ENOSPC when the extension cannot be represented.
 */
static int lazy_dir_extend(uint32_t dir_id, uint32_t wanted)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];

	if (wanted <= dir->clusters) {
		return 0;
	}
	while (dir->clusters < wanted) {
		struct proxy_lazy_partition *part = &lazy.partitions[dir->partition];
		uint32_t contiguous = dir->first_cluster + dir->clusters;
		uint32_t next;

		if (contiguous == part->bump &&
		    (uint64_t)part->bump + 1U <=
		    (uint64_t)part->reserve_first + part->reserve_clusters) {
			part->bump++;
			next = contiguous;
		} else {
			struct proxy_lazy_range occupied;

			if (contiguous < lazy.total_clusters + 2U &&
			    lazy_range_find(contiguous, &occupied) != 0 &&
			    (lazy.bitmap[(contiguous - 2U) / 8U] &
			     (uint8_t)(1U << ((contiguous - 2U) % 8U))) != 0U) {
				/*
				 * The parent entry reports a longer directory and the
				 * host marked the following cluster used: a contiguous
				 * extension, with or without a FAT link.
				 */
				next = contiguous;
			} else {
				/* Otherwise the host may have linked a new cluster on the FAT. */
				next = lazy_fat_next(dir->extension_cluster);
				if (next < PROXY_LAZY_ROOT_CLUSTER_MIN ||
				    next >= lazy.total_clusters + 2U ||
				    lazy_range_find(next, &occupied) == 0) {
					lazy.last_error = -ENOSPC;
					return -ENOSPC;
				}
			}
		}
		lazy_pending_drop_cluster(next);
		if (next != contiguous) {
			dir->chained = true;
			lazy_dir_parent_chained(dir_id);
		}
		if (lazy_range_add(next, 1U, dir_id, PROXY_LAZY_RANGE_DIRECTORY,
		    PROXY_LAZY_INVALID_SLOT, dir->clusters) != 0) {
			return -ENOMEM;
		}
		dir->clusters++;
		dir->extension_cluster = next;
	}
	return 0;
}

/**
 * @brief Apply a host entry that names an entry the cache already has.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index.
 * @param[in] directory   True for a directory.
 * @param[in] host_slot   Absolute first slot.
 * @param[in] first_cluster First cluster the host entry names.
 * @param[in] clusters    Clusters the host size asks for.
 * @param[in] size        File size or directory data length.
 * @param[in] fat_chain   True when the host extent is a FAT chain.
 * @return 0 on success, or a negative errno.
 */
static int lazy_update_entry(uint32_t dir_id, uint32_t entry_index, bool directory,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dirent *item = &dir->entries[entry_index];
	int result = 0;

	if (item->directory != directory) {
		return -EIO;
	}
	item->slot = host_slot;
	if (directory) {
		uint32_t child = lazy_dir_find_child(dir, dir->names + item->name_offset);

		if (child == PROXY_LAZY_INVALID_DIR) {
			return -EIO;
		}
		result = lazy_dir_extend(child, clusters != 0U ? clusters : 1U);
		if (result != 0) {
			return result;
		}
		item->first_cluster = lazy.dirs[child].first_cluster;
		item->clusters = lazy.dirs[child].clusters;
		item->fat_chain = lazy.dirs[child].chained;
	} else if (size == 0U) {
		item->first_cluster = 0U;
		item->clusters = 0U;
		item->fat_chain = false;
	} else if (first_cluster < PROXY_LAZY_ROOT_CLUSTER_MIN) {
		return -EIO;
	} else if (first_cluster != item->first_cluster || fat_chain) {
		/* The host moved the data or linked it through the FAT. */
		item->first_cluster = first_cluster;
		item->clusters = clusters;
		item->host_owned = true;
		item->fat_chain = fat_chain;
	} else if (!item->host_owned && clusters > item->clusters) {
		/*
		 * A synthetic extent cannot grow into the next file's clusters. The
		 * source entry asks for more than the view advertised, which means the
		 * host wrote somewhere this engine did not map.
		 */
		return -ENOSPC;
	} else {
		item->clusters = clusters;
	}
	result = lazy_entry_resize(dir_id, entry_index, size);
	if (result != 0) {
		return result;
	}
	dir->ranges_dirty = true;
	return 0;
}

/**
 * @brief Remove a source object again after a half-applied host create.
 *
 * @param[in] dir_id    Parent directory.
 * @param[in] name      Entry name.
 * @param[in] directory True for a directory.
 */
static void lazy_source_remove(uint32_t dir_id, const char *name, bool directory)
{
	char path[VFS_PATH_MAX];

	if (lazy_child_path(dir_id, name, path, sizeof(path)) != 0) {
		return;
	}
	if (directory) {
		(void)vfs_rmdir(path);
	} else {
		(void)vfs_unlink(path);
	}
}

/**
 * @brief Adopt a host extent for a host-created file or directory.
 *
 * @param[in] dir_id      Parent directory identifier.
 * @param[in] name        Entry name.
 * @param[in] directory   True for a directory.
 * @param[in] host_slot   Absolute first slot.
 * @param[in] first_cluster First cluster the host entry names.
 * @param[in] clusters    Clusters the size asks for.
 * @param[in] size        File size or directory data length.
 * @param[in] fat_chain   True when the host extent is a FAT chain.
 * @return 0 on success, or a negative errno.
 */
static int lazy_create_entry(uint32_t dir_id, const char *name, bool directory,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint32_t extent_first = first_cluster;
	uint32_t extent_clusters = clusters;
	uint32_t inserted;
	int result = lazy_source_create(dir_id, name, directory);

	if (result != 0) {
		return result;
	}
	if (directory) {
		uint32_t child;
		struct proxy_lazy_dir *header;

		if (extent_clusters == 0U) {
			extent_clusters = 1U;
		}
		if (extent_first < PROXY_LAZY_ROOT_CLUSTER_MIN) {
			result = lazy_alloc_clusters(dir->partition, extent_clusters,
				&extent_first, &extent_clusters);
			if (result != 0) {
				lazy_source_remove(dir_id, name, true);
				return result;
			}
		}
		child = lazy_dir_find_child(dir, name);
		if (child == PROXY_LAZY_INVALID_DIR) {
			result = lazy_subdir_reserve(dir);
			if (result != 0) {
				lazy_source_remove(dir_id, name, true);
				return result;
			}
			child = lazy_dir_alloc(name);
			if (child == UINT32_MAX) {
				lazy_source_remove(dir_id, name, true);
				return -ENOSPC;
			}
			lazy.dirs[child].parent = dir_id;
			lazy.dirs[child].partition = dir->partition;
			result = lazy_subdir_append(dir, child);
			if (result != 0) {
				lazy_source_remove(dir_id, name, true);
				return result;
			}
		}
		header = &lazy.dirs[child];
		if (header->range_ready) {
			lazy_range_remove_owner(child, PROXY_LAZY_RANGE_DIRECTORY);
			lazy_cache_evict(child);
		}
		lazy_overlay_clear_dir(child);
		header->first_cluster = extent_first;
		header->clusters = extent_clusters;
		header->extension_cluster = extent_first + extent_clusters - 1U;
		header->extent_ready = true;
		header->range_ready = false;
		header->chained = false;
		result = lazy_range_add(extent_first, extent_clusters, child,
			PROXY_LAZY_RANGE_DIRECTORY, PROXY_LAZY_INVALID_SLOT, 0U);
		if (result != 0) {
			lazy_source_remove(dir_id, name, true);
			return result;
		}
		header->range_ready = true;
		/* Zeroing a fresh host directory is synthesized, not file data. */
		for (uint32_t cluster = extent_first;
		     cluster < extent_first + extent_clusters; cluster++) {
			lazy_pending_drop_cluster(cluster);
		}
	} else if (size != 0U && extent_first < PROXY_LAZY_ROOT_CLUSTER_MIN) {
		lazy_source_remove(dir_id, name, false);
		return -EIO;
	}
	result = lazy_dir_insert(dir, name, directory, host_slot, extent_first,
		extent_clusters, size, true, directory ? false : fat_chain, &inserted);
	if (result != 0) {
		lazy_source_remove(dir_id, name, directory);
		return result;
	}
	dir->ranges_dirty = true;
	return 0;
}

/**
 * @brief Forget the overlay record of one name.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] name   Entry name.
 */
static void lazy_overlay_forget(uint32_t dir_id, const char *name)
{
	struct proxy_lazy_overlay *record = lazy_overlay_find_name(dir_id, name);

	if (record != NULL && record->used) {
		record->used = false;
		if (lazy.overlay_count != 0U) {
			lazy.overlay_count--;
		}
	}
}

/**
 * @brief Apply a host entry that renames an entry the cache already has.
 *
 * @param[in] dir_id      Directory identifier.
 * @param[in] entry_index Entry index of the old name.
 * @param[in] name        New name.
 * @param[in] host_slot   New absolute first slot.
 * @param[in] first_cluster First cluster of the host extent.
 * @param[in] clusters    Clusters the size asks for.
 * @param[in] size        File size or directory data length.
 * @param[in] fat_chain   True when the host extent is a FAT chain.
 * @return 0 on success, or a negative errno.
 */
static int lazy_rename_entry(uint32_t dir_id, uint32_t entry_index, const char *name,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	struct proxy_lazy_dirent *item = &dir->entries[entry_index];
	char old_name[VFS_NAME_MAX + 1];
	char old_path[VFS_PATH_MAX];
	char new_path[VFS_PATH_MAX];
	uint32_t name_offset;
	int result;

	snprintf(old_name, sizeof(old_name), "%s", dir->names + item->name_offset);
	result = lazy_entry_path(dir_id, item, old_path, sizeof(old_path));
	if (result != 0) {
		return result;
	}
	result = lazy_child_path(dir_id, name, new_path, sizeof(new_path));
	if (result != 0) {
		return result;
	}
	result = lazy_apply_deletes_locked();
	if (result != 0) {
		return result;
	}
	result = vfs_rename(old_path, new_path);
	if (result == -EEXIST) {
		result = lazy_apply_deletes_locked();
		if (result == 0) {
			result = vfs_rename(old_path, new_path);
		}
	}
	if (result != 0) {
		return result;
	}
	if (item->directory) {
		uint32_t child = lazy_dir_find_child(dir, old_name);

		if (child != PROXY_LAZY_INVALID_DIR) {
			uint32_t offset = lazy_store_name(name);

			if (offset == UINT32_MAX) {
				return -ENOSPC;
			}
			lazy.dirs[child].name_offset = offset;
			lazy.dirs[child].parent = dir_id;
		}
	}
	lazy_overlay_forget(dir_id, old_name);
	result = lazy_dir_name_copy(dir, name, &name_offset);
	if (result != 0) {
		return result;
	}
	item->name_offset = name_offset;
	item->entries = (uint16_t)lazy_name_entries(name);
	result = lazy_update_entry(dir_id, entry_index, item->directory, host_slot,
		first_cluster, clusters, size, fat_chain);
	if (result != 0) {
		return result;
	}
	return 0;
}

/**
 * @brief Mark the entry at an absolute slot deleted and remember the tombstone.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] slot   Absolute slot of the deleted stream entry.
 * @return 0 on success, or -ENOSPC when the overlay is full.
 */
static int lazy_delete_entry(uint32_t dir_id, uint32_t slot)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint32_t index = lazy_entry_by_slot(dir, slot);
	struct proxy_lazy_dirent *item;
	int result;

	if (index == UINT32_MAX || dir->entries[index].deleted) {
		return 0;
	}
	item = &dir->entries[index];
	result = lazy_overlay_tombstone(dir_id, dir->names + item->name_offset,
		item->slot, item->directory, item->entries);
	if (result != 0) {
		return result;
	}
	item->deleted = true;
	dir->ranges_dirty = true;
	return 0;
}

/**
 * @brief Apply one parsed host entry.
 *
 * @details The entry is matched by name first, then by the cluster it names,
 * which is what turns a clear-and-rewrite at another slot into a rename. An
 * unknown name creates the source object. The overlay records the result so a
 * later cache reload reproduces the host's view.
 *
 * @param[in] dir_id        Parent directory.
 * @param[in] name          Entry name.
 * @param[in] directory     True for a directory.
 * @param[in] first_cluster First cluster named by the host.
 * @param[in] size          File size or directory data length.
 * @param[in] fat_chain     True when the host extent is a FAT chain.
 * @param[in] host_slot     Absolute first slot.
 * @return 0 on success, or a negative errno.
 */
static int lazy_sync_entry(uint32_t dir_id, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, bool fat_chain, uint32_t host_slot)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint32_t clusters = size == 0U ? 0U :
		(uint32_t)((size + PROXY_LAZY_CLUSTER_SIZE - 1U) / PROXY_LAZY_CLUSTER_SIZE);
	uint32_t index = lazy_dir_find_name(dir, name, false);
	int result;

	if (directory && clusters == 0U) {
		clusters = 1U;
	}
	if (index != UINT32_MAX) {
		result = lazy_update_entry(dir_id, index, directory, host_slot,
			first_cluster, clusters, size, fat_chain);
	} else {
		uint32_t old = first_cluster != 0U ?
			lazy_dir_find_cluster(dir, first_cluster, true) : UINT32_MAX;

		if (old != UINT32_MAX && dir->entries[old].directory == directory &&
		    !dir->entries[old].deleted) {
			result = lazy_rename_entry(dir_id, old, name, host_slot,
				first_cluster, clusters, size, fat_chain);
		} else if (old != UINT32_MAX && dir->entries[old].deleted &&
			   dir->entries[old].directory == directory) {
			/* Delete and re-create of the same object in one batch. */
			lazy_overlay_forget(dir_id, dir->names + dir->entries[old].name_offset);
			dir->entries[old].deleted = false;
			result = lazy_rename_entry(dir_id, old, name, host_slot,
				first_cluster, clusters, size, fat_chain);
		} else {
			result = lazy_create_entry(dir_id, name, directory, host_slot,
				first_cluster, clusters, size, fat_chain);
		}
	}
	if (result != 0) {
		pr_warn("lazy proxy: sync %s/%s slot %u size %llu failed: %d\n",
			lazy_dir_name(dir_id), name, host_slot,
			(unsigned long long)size, result);
		lazy.last_error = result;
		return result;
	}
	result = lazy_overlay_put(dir_id, name, host_slot, first_cluster, clusters, size,
		directory, fat_chain);
	if (result != 0) {
		lazy.last_error = result;
		return result;
	}
	return 0;
}

/**
 * @brief Convert UTF-16 name code units back to UTF-8.
 *
 * @param[in]  input    UTF-16 code units.
 * @param[in]  length   Number of code units to convert.
 * @param[out] output   Buffer receiving the UTF-8 string.
 * @param[in]  capacity Size of @p output in bytes.
 * @return Number of bytes written, excluding the terminator.
 */
static size_t lazy_utf8(const uint16_t *input, size_t length, char *output, size_t capacity)
{
	size_t written = 0U;

	for (size_t index = 0U; index < length && input[index] != 0U; index++) {
		uint32_t codepoint = input[index];
		uint8_t bytes[3];
		size_t count;

		if (codepoint < 0x80U) {
			bytes[0] = (uint8_t)codepoint;
			count = 1U;
		} else if (codepoint < 0x800U) {
			bytes[0] = 0xc0U | (uint8_t)(codepoint >> 6);
			bytes[1] = 0x80U | (uint8_t)(codepoint & 0x3fU);
			count = 2U;
		} else {
			bytes[0] = 0xe0U | (uint8_t)(codepoint >> 12);
			bytes[1] = 0x80U | (uint8_t)((codepoint >> 6) & 0x3fU);
			bytes[2] = 0x80U | (uint8_t)(codepoint & 0x3fU);
			count = 3U;
		}
		if (written + count + 1U > capacity) {
			break;
		}
		memcpy(output + written, bytes, count);
		written += count;
	}
	output[written] = '\0';
	return written;
}

/**
 * @brief Extract the name of a host entry from its secondary entries.
 *
 * @param[in]  slots       Window slots.
 * @param[in]  slot        Slot of the stream entry inside the window.
 * @param[in]  secondaries Number of secondary entries in the group.
 * @param[out] name        Buffer receiving the UTF-8 name.
 * @return True when a complete name was decoded.
 */
static bool lazy_entry_name(const uint8_t *slots, uint32_t slot, uint32_t secondaries,
	char name[VFS_NAME_MAX + 1])
{
	uint16_t characters[VFS_NAME_MAX];
	uint32_t length = slots[(slot + 1U) * 32U + 3U];
	uint32_t copied = 0U;

	if (length > VFS_NAME_MAX || slot + secondaries >= PROXY_LAZY_WINDOW_SLOTS) {
		return false;
	}
	for (uint32_t entry = 2U; entry <= secondaries && copied < length; entry++) {
		const uint8_t *name_entry = slots + (slot + entry) * 32U;

		if (name_entry[0] != 0xc1U) {
			return false;
		}
		for (uint32_t character = 0U; character < 15U && copied < length; character++) {
			characters[copied++] = (uint16_t)(name_entry[2U + character * 2U] |
				((uint16_t)name_entry[3U + character * 2U] << 8));
		}
	}
	lazy_utf8(characters, copied, name, VFS_NAME_MAX + 1U);
	return copied == length;
}

/**
 * @brief Read a little-endian 16-bit value.
 *
 * @param[in] bytes Two bytes in little-endian order.
 * @return The decoded value.
 */
static uint16_t lazy_get_le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

/**
 * @brief Read a little-endian 32-bit value.
 *
 * @param[in] bytes Four bytes in little-endian order.
 * @return The decoded value.
 */
static uint32_t lazy_get_le32(const uint8_t *bytes)
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
static uint64_t lazy_get_le64(const uint8_t *bytes)
{
	return (uint64_t)lazy_get_le32(bytes) |
		((uint64_t)lazy_get_le32(bytes + 4U) << 32);
}

/**
 * @brief Parse one directory sector the host wrote and apply its entries.
 *
 * @details A window of [kept head of a cut group][new sector] lets an entry
 * group that straddles the sector edge be completed. Deletions are seen by
 * comparing the new sector with the previous rendering. Groups whose bytes did
 * not change are skipped, so a host that rewrites a whole sector only pays for
 * what it changed.
 *
 * @param[in] dir_id Directory identifier.
 * @param[in] offset Byte offset of the sector inside the directory extent.
 * @param[in] data   New sector contents.
 * @return 0 on success, or a negative errno.
 */
static int lazy_write_directory(uint32_t dir_id, uint64_t offset,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	uint8_t *window = lazy.directory_window;
	uint8_t *previous = lazy.directory_previous;
	uint32_t touched[PROXY_LAZY_SLOTS_PER_SECTOR + 1U];
	uint32_t touched_count = 0U;
	uint32_t head_slots = 0U;
	uint32_t window_slots;
	int result = 0;

	if (lazy.split_slots != 0U && lazy.split_dir == dir_id &&
	    lazy.split_offset + PROXY_LAZY_SECTOR_SIZE == offset) {
		head_slots = lazy.split_slots;
		memcpy(window, lazy.split_head, (size_t)head_slots * 32U);
	}
	lazy.split_slots = 0U;
	memset(window + (size_t)head_slots * 32U, 0,
		sizeof(lazy.directory_window) - (size_t)head_slots * 32U);
	memcpy(window + (size_t)head_slots * 32U, data, PROXY_LAZY_SECTOR_SIZE);
	window_slots = head_slots + PROXY_LAZY_SLOTS_PER_SECTOR;
	result = lazy_render_directory(dir_id, offset, previous);
	if (result != 0) {
		return result;
	}
	for (uint32_t slot = 0U; slot < window_slots; slot++) {
		const uint8_t *entry = window + slot * 32U;

		if (slot >= head_slots && previous[(slot - head_slots) * 32U] == 0x85U &&
		    entry[0] == 0x05U) {
			result = lazy_delete_entry(dir_id,
				(uint32_t)(offset / 32U) + slot - head_slots);
			if (result != 0) {
				return result;
			}
		}
		if (entry[0] == 0x85U) {
			uint32_t secondaries = entry[1];
			const uint8_t *stream;
			char name[VFS_NAME_MAX + 1];
			bool directory;
			bool fat_chain;
			uint32_t first_cluster;
			uint64_t data_length;
			uint64_t valid_length;
			uint32_t host_slot;

			if (secondaries < 2U || secondaries > 18U) {
				continue;
			}
			if (slot + secondaries >= window_slots) {
				lazy.split_slots = window_slots - slot;
				lazy.split_dir = dir_id;
				lazy.split_offset = offset;
				memcpy(lazy.split_head, window + slot * 32U,
					(size_t)lazy.split_slots * 32U);
				break;
			}
			if (window[(slot + 1U) * 32U] != 0xc0U ||
			    !lazy_entry_name(window, slot, secondaries, name)) {
				continue;
			}
			if (slot >= head_slots &&
			    slot - head_slots + secondaries < PROXY_LAZY_SLOTS_PER_SECTOR &&
			    memcmp(window + slot * 32U,
				previous + (slot - head_slots) * 32U,
				(size_t)(secondaries + 1U) * 32U) == 0) {
				slot += secondaries;
				continue;
			}
			stream = window + (slot + 1U) * 32U;
			directory = (lazy_get_le16(entry + 4U) & 0x10U) != 0U;
			fat_chain = (stream[1] & 0x02U) == 0U;
			first_cluster = lazy_get_le32(stream + 20U);
			data_length = lazy_get_le64(stream + 24U);
			valid_length = lazy_get_le64(stream + 8U);
			/*
			 * A host reserves whole clusters in DataLength; only
			 * ValidDataLength exists. Taking the larger one would pad the
			 * source file with zeros.
			 */
			if (!directory && valid_length < data_length) {
				data_length = valid_length;
			}
			host_slot = (uint32_t)(offset / 32U) - head_slots + slot;
			result = lazy_sync_entry(dir_id, name, directory, first_cluster,
				data_length, fat_chain, host_slot);
			if (result != 0) {
				return result;
			}
			if (touched_count < sizeof(touched) / sizeof(touched[0])) {
				touched[touched_count++] = host_slot;
			}
			slot += secondaries;
		}
	}
	if (dir->ranges_dirty) {
		lazy_dir_sort(dir);
		result = lazy_rebuild_data_ranges(dir_id);
		if (result != 0) {
			return result;
		}
		result = lazy_claim_pending_dir(dir_id);
		if (result != 0) {
			return result;
		}
	}
	/* A host writes whole sectors; the source file keeps the host's exact size. */
	for (uint32_t index = 0U; index < touched_count; index++) {
		uint32_t entry_index = lazy_entry_by_slot(dir, touched[index]);

		if (entry_index == UINT32_MAX) {
			continue;
		}
		{
			const struct proxy_lazy_dirent *item = &dir->entries[entry_index];

			if (!item->directory && !item->deleted) {
				result = lazy_truncate_entry(dir_id, entry_index, item->size);

				if (result != 0) {
					return result;
				}
			}
		}
	}
	dir->sizes_dirty = false;
	return 0;
}

/**
 * @brief Report whether a cluster continues a directory through a host FAT chain.
 *
 * @param[in] cluster Cluster being written.
 * @return The directory identifier, or PROXY_LAZY_INVALID_DIR.
 */
static uint32_t lazy_directory_extension(uint32_t cluster)
{
	for (uint32_t index = 1U; index < lazy.dir_count; index++) {
		struct proxy_lazy_dir *dir = &lazy.dirs[index];

		if (dir->is_root || !dir->extent_ready) {
			continue;
		}
		if (dir->extension_cluster != 0U &&
		    lazy_fat_next(dir->extension_cluster) == cluster) {
			return index;
		}
	}
	return PROXY_LAZY_INVALID_DIR;
}

/**
 * @brief Map one cluster the host appended to a directory.
 *
 * @param[in] dir_id  Directory identifier.
 * @param[in] cluster Cluster to map.
 * @return 0 on success, or a negative errno.
 */
static int lazy_map_directory_cluster(uint32_t dir_id, uint32_t cluster)
{
	struct proxy_lazy_dir *dir = &lazy.dirs[dir_id];
	int result;

	if (cluster != dir->first_cluster + dir->clusters) {
		dir->chained = true;
		lazy_dir_parent_chained(dir_id);
	}
	result = lazy_range_add(cluster, 1U, dir_id, PROXY_LAZY_RANGE_DIRECTORY,
		PROXY_LAZY_INVALID_SLOT, dir->clusters);

	if (result != 0) {
		return result;
	}
	lazy_pending_drop_cluster(cluster);
	dir->clusters++;
	dir->extension_cluster = cluster;
	return 0;
}

/**
 * @brief Apply one host sector write.
 *
 * @details The sector is routed by the volume area it belongs to: system areas
 * are read-only, the bitmap is updated in RAM, a directory sector is parsed, a
 * file sector is written through to its source file, and a sector in the free
 * tail waits in the pending table until its entry arrives.
 *
 * @param[in] sector_number Logical sector number inside the view.
 * @param[in] data          Sector contents.
 * @return 0 on success, or a negative errno.
 */
static int lazy_write_sector(uint64_t sector_number, const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	struct proxy_lazy_range range;
	uint64_t relative;
	uint32_t cluster;
	uint16_t sector_in_cluster;

	if (sector_number >= lazy.total_sectors) {
		return -EINVAL;
	}
	if (sector_number < PROXY_LAZY_BOOT_SECTORS) {
		return 0; /* The geometry is fixed; volume flags are not used. */
	}
	if (sector_number < PROXY_LAZY_FAT_OFFSET) {
		return 0;
	}
	if (sector_number < PROXY_LAZY_FAT_OFFSET + lazy.fat_sectors) {
		int result = lazy_fat_write((uint32_t)(sector_number - PROXY_LAZY_FAT_OFFSET), data);

		return result;
	}
	if (sector_number < lazy.data_start) {
		return 0;
	}
	relative = sector_number - lazy.data_start;
	cluster = (uint32_t)(relative / PROXY_LAZY_SECTORS_PER_CLUSTER) +
		PROXY_LAZY_ROOT_CLUSTER_MIN;
	sector_in_cluster = (uint16_t)(relative % PROXY_LAZY_SECTORS_PER_CLUSTER);
	if (cluster >= lazy.bitmap_cluster && cluster < lazy.bitmap_cluster + lazy.bitmap_clusters) {
		uint64_t bitmap_offset = (uint64_t)(cluster - lazy.bitmap_cluster) *
			PROXY_LAZY_CLUSTER_SIZE +
			(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;
		size_t length = bitmap_offset < lazy.bitmap_bytes ?
			lazy.bitmap_bytes - (size_t)bitmap_offset : 0U;

		if (length > PROXY_LAZY_SECTOR_SIZE) {
			length = PROXY_LAZY_SECTOR_SIZE;
		}
		if (length != 0U) {
			for (size_t index = 0U; index < length; index++) {
				uint32_t byte_offset = (uint32_t)bitmap_offset + (uint32_t)index;

				for (uint32_t bit = 0U; bit < 8U; bit++) {
					uint32_t number = 2U + byte_offset * 8U + bit;
					bool used = (data[index] & (uint8_t)(1U << bit)) != 0U;
					bool was = (lazy.bitmap[byte_offset] &
						(uint8_t)(1U << bit)) != 0U;

					if (number < lazy.tail_first) {
						/* The reservations stay used no matter what the host wrote. */
						lazy.bitmap[byte_offset] |= (uint8_t)(1U << bit);
					} else if (number < lazy.total_clusters + 2U) {
						if (used) {
							lazy.bitmap[byte_offset] |= (uint8_t)(1U << bit);
						} else {
							lazy.bitmap[byte_offset] &= (uint8_t)~(1U << bit);
						}
						if (was && !used) {
							lazy_pending_drop_cluster(number);
							lazy_range_remove_data_cluster(number);
							lazy_forget_freed_cluster(number);
						}
					} else {
						lazy.bitmap[byte_offset] &= (uint8_t)~(1U << bit);
					}
				}
			}
		}
		lazy.clean = false;
		return 0;
	}
	if (cluster >= lazy.upcase_cluster && cluster < lazy.upcase_cluster + lazy.upcase_clusters) {
		return -EROFS;
	}
	if (lazy_range_find(cluster, &range) == 0) {
		if (range.kind == PROXY_LAZY_RANGE_DIRECTORY) {
			uint64_t offset = ((uint64_t)range.offset +
				(cluster - range.base)) * PROXY_LAZY_CLUSTER_SIZE +
				(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE;

			if (range.dir_id == PROXY_LAZY_ROOT_ID) {
				uint8_t current[PROXY_LAZY_SECTOR_SIZE];
				uint32_t first_slot = (uint32_t)(offset / 32U);

				memset(current, 0, sizeof(current));
				for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
					int result = lazy_render_root_slot(first_slot + slot,
						current + slot * 32U);

					if (result != 0) {
						return result;
					}
				}
				/*
				 * The system and partition-folder slots are synthesized and stay
				 * read-only. Slots from root_host_slot on are the host's metadata
				 * area (System Volume Information and anything else Windows adds);
				 * accept those writes without persisting them, because the root
				 * entry is already rendered from a real backing directory.
				 */
				for (uint32_t slot = 0U; slot < PROXY_LAZY_SLOTS_PER_SECTOR; slot++) {
					uint32_t absolute = first_slot + slot;

					if (absolute < lazy.root_host_slot &&
					    memcmp(current + slot * 32U, data + slot * 32U, 32U) != 0) {
						pr_warn("lazy: root system slot %u changed\n", absolute);
						return -EROFS;
					}
				}
				return 0;
			}
			return lazy_write_directory(range.dir_id, offset, data);
		}
		{
			int result = lazy_dir_load(range.dir_id);
			uint32_t index;

			if (result != 0) {
				return result;
			}
			index = lazy_entry_by_slot(&lazy.dirs[range.dir_id], range.slot);
			if (index == UINT32_MAX) {
				return -EIO;
			}
			return lazy_write_bytes(range.dir_id, index,
				((uint64_t)range.offset + (cluster - range.base)) *
				PROXY_LAZY_CLUSTER_SIZE +
				(uint64_t)sector_in_cluster * PROXY_LAZY_SECTOR_SIZE,
				data, PROXY_LAZY_SECTOR_SIZE);
		}
	}
	/*
	 * A cluster the host appended to a directory is mapped when the parent
	 * entry reports the larger data length, or when the host wrote a FAT link
	 * from the directory's last cluster. Adjacency alone is not evidence: a
	 * host-created file's extent often starts right after a directory extent.
	 */
	{
		uint32_t dir_id = lazy_directory_extension(cluster);

		if (dir_id != PROXY_LAZY_INVALID_DIR) {
			int result = lazy_map_directory_cluster(dir_id, cluster);

			if (result != 0) {
				return result;
			}
			return lazy_write_directory(dir_id,
				(uint64_t)(lazy.dirs[dir_id].clusters - 1U), data);
		}
	}
	/*
	 * A cluster inside a reservation may be unowned because the host freed and
	 * reused it before rewriting the directory entry (truncate-then-write).
	 * Keep the data until an entry claims the extent; if none ever does, the
	 * bounded pending table drops it without touching a source file.
	 */
	return lazy_pending_store(cluster, sector_in_cluster, data);
}

/* ---- view geometry ---- */

/**
 * @brief Compute the reservation of one selected partition.
 *
 * @details ext volumes reserve one view cluster per used 32 KiB of data plus
 * one per used inode, which pays for the 32 KiB rounding of small files and
 * for the directory extents. FAT volumes have no inode count, so one source
 * cluster per used source cluster plus a fixed file allowance is reserved.
 *
 * @param[in,out] part Partition to size.
 * @return 0 on success, or -EFBIG when the reservation does not fit a cluster count.
 */
static int lazy_compute_reservation(struct proxy_lazy_partition *part)
{
	uint64_t used_bytes;
	uint64_t used_clusters;
	uint64_t reservation;

	if (part->status != 0) {
		part->reserve_clusters = 8U;
		return 0;
	}
	used_bytes = part->volume.total_bytes > part->volume.free_bytes ?
		part->volume.total_bytes - part->volume.free_bytes : 0U;
	used_clusters = (used_bytes + PROXY_LAZY_CLUSTER_SIZE - 1U) / PROXY_LAZY_CLUSTER_SIZE;
	if (strncmp(part->volume.fstype, "ext", 3U) == 0) {
		reservation = used_clusters + part->volume.used_inodes + 8U;
	} else {
		uint64_t counted = used_clusters;

		/*
		 * The larger of the view cluster count and the source cluster count
		 * covers both a small-cluster volume with many tiny files and a
		 * large-cluster volume whose view rounds every file up to 32 KiB.
		 */
		if (part->volume.cluster_bytes != 0U) {
			uint64_t source_clusters = used_bytes / part->volume.cluster_bytes;

			if (source_clusters > counted) {
				counted = source_clusters;
			}
		}
		if (counted * 2U > counted + CONFIG_STORAGE_PROXY_LAZY_FILE_ALLOWANCE) {
			reservation = counted * 2U;
		} else {
			reservation = counted + CONFIG_STORAGE_PROXY_LAZY_FILE_ALLOWANCE;
		}
		reservation += 8U;
	}
	if (reservation < 8U) {
		reservation = 8U;
	}
	if (reservation > 0x7ffffffdU) {
		return -EFBIG;
	}
	part->reserve_clusters = (uint32_t)reservation;
	return 0;
}

/**
 * @brief Size the view, build its system areas and mark every reservation used.
 *
 * @details The total cluster count follows from the up-case table, the root
 * directory, the partition reservations and the free tail. The allocation
 * bitmap itself occupies clusters too, so the count is solved iteratively. The
 * bitmap is allocated once and has every cluster up to the tail marked used;
 * nothing changes it later.
 *
 * @return 0 on success, or a negative errno.
 */
static int lazy_compute_geometry(void)
{
	uint64_t base = (uint64_t)lazy.root_clusters + lazy.tail_clusters;
	uint64_t total;
	uint64_t bitmap_bytes;
	uint64_t bitmap_clusters;
	uint32_t next_cluster;
	int result;

	result = lazy_build_upcase();
	if (result != 0) {
		return result;
	}
	lazy.upcase_clusters = (lazy.upcase_bytes + PROXY_LAZY_CLUSTER_SIZE - 1U) /
		PROXY_LAZY_CLUSTER_SIZE;
	base += lazy.upcase_clusters;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		if (lazy.partitions[partition].selected) {
			base += lazy.partitions[partition].reserve_clusters;
		}
	}
	total = base;
	for (uint32_t attempt = 0U; attempt < 8U; attempt++) {
		uint64_t next;

		bitmap_bytes = (total + 7U) / 8U;
		bitmap_clusters = (bitmap_bytes + PROXY_LAZY_CLUSTER_SIZE - 1U) /
			PROXY_LAZY_CLUSTER_SIZE;
		next = base + bitmap_clusters;
		if (next == total) {
			break;
		}
		total = next;
	}
	if (total > 0x7ffffffdU) {
		return -EFBIG;
	}
	lazy.total_clusters = (uint32_t)total;
	lazy.bitmap_bytes = (uint32_t)((total + 7U) / 8U);
	lazy.bitmap_clusters = (uint32_t)(((uint64_t)lazy.bitmap_bytes +
		PROXY_LAZY_CLUSTER_SIZE - 1U) / PROXY_LAZY_CLUSTER_SIZE);
	lazy.bitmap = lazy_calloc(1U, lazy.bitmap_bytes);
	if (lazy.bitmap == NULL) {
		return -ENOMEM;
	}
	lazy.fat_sectors = (uint32_t)(((uint64_t)(lazy.total_clusters + 2U) * 4U +
		PROXY_LAZY_SECTOR_SIZE - 1U) / PROXY_LAZY_SECTOR_SIZE);
	lazy.data_start = (PROXY_LAZY_FAT_OFFSET + lazy.fat_sectors +
		PROXY_LAZY_SECTORS_PER_CLUSTER_U32 - 1U) & ~(PROXY_LAZY_SECTORS_PER_CLUSTER_U32 - 1U);
	lazy.total_sectors = lazy.data_start +
		(uint64_t)lazy.total_clusters * PROXY_LAZY_SECTORS_PER_CLUSTER_U32;
	if (lazy.total_sectors > UINT32_MAX) {
		return -EFBIG;
	}
	lazy.bitmap_cluster = PROXY_LAZY_ROOT_CLUSTER_MIN;
	lazy.upcase_cluster = lazy.bitmap_cluster + lazy.bitmap_clusters;
	lazy.root_cluster = lazy.upcase_cluster + lazy.upcase_clusters;
	next_cluster = lazy.root_cluster + lazy.root_clusters;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];

		if (!part->selected) {
			continue;
		}
		part->reserve_first = next_cluster;
		part->bump = next_cluster;
		next_cluster += part->reserve_clusters;
	}
	lazy.tail_first = next_cluster;
	/* The bitmap is final before the disk exists: mark every reservation used. */
	for (uint32_t cluster = PROXY_LAZY_ROOT_CLUSTER_MIN; cluster < lazy.tail_first; cluster++) {
		uint32_t bit = cluster - PROXY_LAZY_ROOT_CLUSTER_MIN;

		lazy.bitmap[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
	}
	lazy.dirs[PROXY_LAZY_ROOT_ID].first_cluster = lazy.root_cluster;
	lazy.dirs[PROXY_LAZY_ROOT_ID].clusters = lazy.root_clusters;
	lazy.dirs[PROXY_LAZY_ROOT_ID].extent_ready = true;
	result = lazy_range_add(lazy.root_cluster, lazy.root_clusters, PROXY_LAZY_ROOT_ID,
		PROXY_LAZY_RANGE_DIRECTORY, PROXY_LAZY_INVALID_SLOT, 0U);
	if (result == 0) {
		lazy.dirs[PROXY_LAZY_ROOT_ID].range_ready = true;
	}
	return result;
}

/* ---- boot region ---- */

/**
 * @brief Build one sector of the exFAT boot region.
 *
 * @details Records 1 through 8 are empty sectors with only the 0x55AA
 * signature. Record 0 holds the main boot sector; the used-percentage byte is
 * computed from the clusters the reservations hold.
 *
 * @param[in]  record Boot record number, 0 through 11.
 * @param[out] sector Sector buffer to fill.
 */
static void lazy_boot_record(uint32_t record, uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
	if (record >= 1U && record <= 8U) {
		sector[510] = 0x55U;
		sector[511] = 0xaaU;
		return;
	}
	if (record != 0U) {
		return;
	}
	sector[0] = 0xebU;
	sector[1] = 0x76U;
	sector[2] = 0x90U;
	memcpy(sector + 3U, "EXFAT   ", 8U);
	lazy_put_le64(sector + 64U, 0U);
	lazy_put_le64(sector + 72U, lazy.total_sectors);
	lazy_put_le32(sector + 80U, PROXY_LAZY_FAT_OFFSET);
	lazy_put_le32(sector + 84U, lazy.fat_sectors);
	lazy_put_le32(sector + 88U, lazy.data_start);
	lazy_put_le32(sector + 92U, lazy.total_clusters);
	lazy_put_le32(sector + 96U, lazy.root_cluster);
	lazy_put_le32(sector + 100U, 0x4c5a5956U);
	lazy_put_le16(sector + 104U, 0x0100U);
	lazy_put_le16(sector + 106U, 0U);
	sector[108] = 9U;
	sector[109] = 6U;
	sector[110] = 1U;
	sector[111] = 0x80U;
	if (lazy.total_clusters == 0U) {
		sector[112] = 0U;
	} else {
		uint32_t used = lazy.tail_first - PROXY_LAZY_ROOT_CLUSTER_MIN;
		uint32_t percent = used * 100U / lazy.total_clusters;

		sector[112] = (uint8_t)(percent < 100U ? percent : 100U);
	}
	sector[120] = 0xebU;
	sector[121] = 0xfeU;
	sector[510] = 0x55U;
	sector[511] = 0xaaU;
}

/**
 * @brief Compute the checksum of the first eleven boot records.
 *
 * @details The volume flags and the used-percentage byte are excluded, as the
 * exFAT specification requires.
 *
 * @return The boot region checksum.
 */
static uint32_t lazy_boot_checksum(void)
{
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE];
	uint32_t checksum = 0U;

	for (uint32_t record = 0U; record < 11U; record++) {
		lazy_boot_record(record, sector);
		for (uint32_t index = 0U; index < PROXY_LAZY_SECTOR_SIZE; index++) {
			if (record == 0U && (index == 106U || index == 107U || index == 112U)) {
				continue;
			}
			checksum = lazy_checksum32(checksum, sector[index]);
		}
	}
	return checksum;
}

/**
 * @brief FAT chain value for one cluster of a system area.
 *
 * @param[in] cluster Cluster number.
 * @param[in] first   First cluster of the system area.
 * @param[in] count   Number of clusters in the system area.
 * @return The chain value, or zero when the cluster is not in the area.
 */
static uint32_t lazy_system_chain(uint32_t cluster, uint32_t first, uint32_t count)
{
	if (cluster < first || cluster >= first + count) {
		return 0U;
	}
	return cluster + 1U < first + count ? cluster + 1U : PROXY_LAZY_EOC;
}

/**
 * @brief Return the FAT entry for a cluster.
 *
 * @details The bitmap, the up-case table and the root directory are chained in
 * their own runs. Every other cluster returns zero, because objects are
 * contiguous NoFatChain streams.
 *
 * @param[in] cluster Cluster number.
 * @return The chain value stored in the FAT.
 */
static uint32_t lazy_fat_entry(uint32_t cluster)
{
	uint32_t value;

	if (cluster == 0U) {
		return 0xfffffff8U;
	}
	if (cluster == 1U) {
		return PROXY_LAZY_EOC;
	}
	/* Extents the host chained on the FAT are served back from the write log. */
	value = lazy_fat_next(cluster);
	if (value != 0U) {
		return value;
	}
	value = lazy_system_chain(cluster, lazy.bitmap_cluster, lazy.bitmap_clusters);
	if (value != 0U) {
		return value;
	}
	value = lazy_system_chain(cluster, lazy.upcase_cluster, lazy.upcase_clusters);
	if (value != 0U) {
		return value;
	}
	if (cluster >= lazy.root_cluster && cluster < lazy.root_cluster + lazy.root_clusters) {
		return lazy_system_chain(cluster, lazy.root_cluster, lazy.root_clusters);
	}
	return 0U;
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
static int lazy_read_sector(uint64_t sector_number, uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
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
static int lazy_disk_read(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count)
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

/**
 * @brief Carry out the deletes the host requested.
 *
 * @details Files go first, then directories are retried until no more become
 * empty, so a recursively deleted tree is removed in one pass. A directory
 * that is still not empty stays for the next flush rather than failing the
 * host. A name that is already gone counts as deleted.
 *
 * @return 0 on success, or the first hard error.
 */
static int lazy_apply_deletes_locked(void)
{
	int result = 0;
	bool progress;

	if (lazy.overlay == NULL) {
		return 0;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		struct proxy_lazy_overlay *record = &lazy.overlay[index];
		char path[VFS_PATH_MAX];
		int deleted;

		if (!record->used || !record->deleted || record->applied ||
		    record->directory) {
			continue;
		}
		if (lazy_child_path(record->dir_id, lazy.overlay_names + record->name_offset,
		    path, sizeof(path)) != 0) {
			record->used = false;
			continue;
		}
		deleted = vfs_unlink(path);
		if (deleted != 0 && deleted != -ENOENT && result == 0) {
			result = deleted;
		}
		if (deleted == 0 || deleted == -ENOENT) {
			record->used = false;
		}
	}
	do {
		progress = false;
		for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
			struct proxy_lazy_overlay *record = &lazy.overlay[index];
			char path[VFS_PATH_MAX];
			int deleted;

			if (!record->used || !record->deleted || record->applied ||
			    !record->directory) {
				continue;
			}
			if (lazy_child_path(record->dir_id,
			    lazy.overlay_names + record->name_offset, path,
			    sizeof(path)) != 0) {
				record->used = false;
				continue;
			}
			deleted = vfs_rmdir(path);
			if (deleted == 0 || deleted == -ENOENT) {
				record->used = false;
				progress = true;
			} else if (deleted != -ENOTEMPTY && result == 0) {
				result = deleted;
			}
		}
	} while (progress && result == 0);
	lazy.overlay_count = 0U;
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX; index++) {
		if (lazy.overlay[index].used) {
			lazy.overlay_count++;
		}
	}
	return result;
}

/**
 * @brief Enforce the announced size of every source file the host wrote.
 *
 * @details A host writes whole sectors, so the source file is usually longer
 * than the ValidDataLength it announced. Only directories whose file data was
 * written are visited.
 *
 * @return 0 on success, or the first error.
 */
static int lazy_enforce_sizes_locked(void)
{
	int result = 0;

	for (uint32_t index = 0U; index < lazy.dir_count; index++) {
		struct proxy_lazy_dir *dir = &lazy.dirs[index];

		if (!dir->cache_ready || !dir->sizes_dirty || dir->entries == NULL) {
			continue;
		}
		for (uint32_t entry = 0U; entry < dir->entry_count; entry++) {
			const struct proxy_lazy_dirent *item = &dir->entries[entry];

			if (!item->directory && !item->deleted && item->clusters != 0U) {
				int resized = lazy_truncate_entry(index, entry, item->size);

				if (resized != 0 && result == 0) {
					result = resized;
				}
			}
		}
		dir->sizes_dirty = false;
	}
	return result;
}

/**
 * @brief Flush queued writes, deferred deletes and the source mounts.
 *
 * @details Idempotent: a successful pass sets @c clean, and a second call does
 * nothing until the host writes again.
 *
 * @return 0 on success, or a negative errno.
 */
static int lazy_flush_locked(void)
{
	int result = 0;

	if (lazy.clean) {
		return 0;
	}
	result = lazy_enforce_sizes_locked();
	if (result == 0) {
		result = lazy_apply_deletes_locked();
	}
	lazy_write_cache_drop();
	for (uint32_t partition = 0U; result == 0 &&
	     partition < PROXY_LAZY_PARTITIONS; partition++) {
		if (lazy.partitions[partition].mounted) {
			result = vfs_sync_mount(lazy.partitions[partition].mount);
		}
	}
	if (result == 0) {
		lazy.clean = true;
		lazy.flush_failed = false;
	} else {
		lazy.flush_failed = true;
		pr_warn("lazy proxy: flush failed: %d\n", result);
	}
	return result;
}

/**
 * @brief Block device write callback of the lazy view.
 *
 * @details The whole request runs under the view lock, one sector at a time.
 * @c lazy_write_sector() routes each sector: system areas are read-only, the
 * bitmap is updated, directory sectors are parsed, file sectors are written to
 * their source file, and free tail sectors wait for their entry.
 *
 * @param[in] disk   Disk being written; unused.
 * @param[in] sector First logical sector.
 * @param[in] buffer Source of the sectors.
 * @param[in] count  Number of sectors.
 * @return 0 on success, or the first failing sector's error.
 */
static int lazy_disk_write(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count)
{
	const uint8_t *input = buffer;
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
	if (sector > lazy.total_sectors ||
	    (uint64_t)count > lazy.total_sectors - sector) {
		mutex_unlock(&lazy.lock);
		lazy_armed_leave();
		return -EINVAL;
	}
	lazy.clean = false;
	for (uint32_t index = 0U; index < count; index++) {
		result = lazy_write_sector(sector + index,
			input + (size_t)index * PROXY_LAZY_SECTOR_SIZE);
		if (result != 0) {
			break;
		}
	}
	lazy.sectors_written += count;
	if (result != 0) {
		uint64_t relative = sector >= lazy.data_start ? sector - lazy.data_start : 0U;
		uint32_t cluster = sector >= lazy.data_start ?
			(uint32_t)(relative / PROXY_LAZY_SECTORS_PER_CLUSTER) + 2U : 0U;

		pr_warn("lazy proxy: write at sector %llu cluster %u failed: %d\n",
			(unsigned long long)sector, cluster, result);
	}
	mutex_unlock(&lazy.lock);
	lazy_armed_leave();
	return result;
}

/**
 * @brief Block device flush callback of the lazy view.
 *
 * @param[in] disk Disk being flushed; unused.
 * @return 0 on success, or -ENODEV after stop began.
 */
static int lazy_disk_flush(struct gendisk *disk)
{
	int result;

	(void)disk;
	if (!lazy_armed_enter()) {
		return -ENODEV;
	}
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result == 0) {
		result = lazy_flush_locked();
		mutex_unlock(&lazy.lock);
	}
	lazy_armed_leave();
	return result;
}

/**
 * @brief Block device free callback of the lazy view.
 *
 * @param[in] disk Disk being freed; unused.
 */
static void lazy_disk_free(struct gendisk *disk)
{
	(void)disk;
}

/** @brief Block device operations the lazy view registers under @c cardview1. */
static const struct block_device_operations lazy_disk_operations = {
	.read = lazy_disk_read,
	.write = lazy_disk_write,
	.flush = lazy_disk_flush,
	.free_disk = lazy_disk_free,
};

/* ---- start and stop ---- */

/**
 * @brief Drop cached state and unmount every source partition.
 *
 * @details The caller has already removed the block device, so nothing can
 * reach the view while its buffers are freed.
 */
static void lazy_release(void)
{
	lazy_write_cache_drop();
	if (lazy.cached_fd >= 0) {
		(void)vfs_close(lazy.cached_fd);
		lazy.cached_fd = -1;
	}
	if (lazy.dirs != NULL) {
		for (uint32_t index = 0U; index < lazy.dir_count; index++) {
			lazy_free(lazy.dirs[index].entries);
			lazy_free(lazy.dirs[index].names);
			lazy_free(lazy.dirs[index].subdirs);
		}
		lazy_free(lazy.dirs);
	}
	lazy.dirs = NULL;
	lazy.dir_count = 0U;
	lazy.dir_capacity = 0U;
	lazy_free(lazy.name_arena);
	lazy.name_arena = NULL;
	lazy.name_used = 0U;
	lazy_free(lazy.ranges);
	lazy.ranges = NULL;
	lazy.range_count = 0U;
	lazy.range_capacity = 0U;
	lazy_free(lazy.bitmap);
	lazy.bitmap = NULL;
	lazy_free(lazy.upcase);
	lazy.upcase = NULL;
	lazy_free(lazy.overlay);
	lazy.overlay = NULL;
	lazy_free(lazy.overlay_names);
	lazy.overlay_names = NULL;
	lazy_free(lazy.pending);
	lazy.pending = NULL;
	lazy_free(lazy.fat_log);
	lazy.fat_log = NULL;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		if (lazy.partitions[partition].mounted) {
			(void)vfs_umount(lazy.partitions[partition].mount);
			lazy.partitions[partition].mounted = false;
		}
	}
	lazy.bitmap_bytes = 0U;
	lazy.bitmap_clusters = 0U;
	lazy.upcase_bytes = 0U;
	lazy.upcase_clusters = 0U;
	lazy.upcase_checksum = 0U;
	lazy.total_clusters = 0U;
	lazy.total_sectors = 0U;
	lazy.data_start = 0U;
	lazy.fat_sectors = 0U;
	lazy.bitmap_cluster = 0U;
	lazy.root_cluster = 0U;
	lazy.root_clusters = 0U;
	lazy.tail_first = 0U;
	lazy.tail_clusters = 0U;
	lazy.root_entry_count = 0U;
	lazy.overlay_count = 0U;
	lazy.overlay_name_used = 0U;
	lazy.pending_count = 0U;
	lazy.fat_count = 0U;
	lazy.split_slots = 0U;
	lazy.split_dir = PROXY_LAZY_INVALID_DIR;
	lazy.split_offset = 0U;
	lazy.write_fd = -1;
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.write_position = 0U;
	lazy.cached_fd = -1;
	lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	lazy.cached_slot = PROXY_LAZY_INVALID_DIR;
	lazy.cached_position = 0U;
	lazy.cache_entries = 0U;
	lazy.cache_bytes = 0U;
	lazy.lru_head = PROXY_LAZY_INVALID_DIR;
	lazy.lru_tail = PROXY_LAZY_INVALID_DIR;
	lazy.truncated = false;
	lazy.last_error = 0;
	lazy.evictions = 0U;
	lazy.sectors_read = 0U;
	lazy.sectors_written = 0U;
	lazy.file_bytes = 0U;
	lazy.file_bytes_written = 0U;
	lazy.clean = true;
	lazy.flush_failed = false;
}

/**
 * @brief Build the partition folders and their root entries.
 *
 * @return 0 on success, or -ENOSPC when the header table or name arena is full.
 */
static int lazy_create_partition_folders(void)
{
	uint32_t slot = PROXY_LAZY_ROOT_SYSTEM_SLOTS;

	lazy.root_entry_count = 0U;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];
		char name[16];
		uint32_t identifier;

		if (!part->selected) {
			continue;
		}
		snprintf(name, sizeof(name), "partition%u", partition + 1U);
		identifier = lazy_dir_alloc(name);
		if (identifier == UINT32_MAX) {
			return -ENOSPC;
		}
		lazy.dirs[identifier].parent = PROXY_LAZY_ROOT_ID;
		lazy.dirs[identifier].partition = partition;
		lazy.dirs[identifier].is_partition_root = true;
		part->dir_id = identifier;
		lazy.root_entries[lazy.root_entry_count].dir_id = identifier;
		lazy.root_entries[lazy.root_entry_count].slot = slot;
		lazy.root_entries[lazy.root_entry_count].entries = lazy_name_entries(name);
		slot += lazy_name_entries(name);
		lazy.root_entry_count++;
	}
	lazy.root_host_slot = slot;
#ifndef STORAGE_PROXY_HOST_TEST
	/*
	 * Windows creates "System Volume Information" in the root of every volume
	 * it mounts read-write. Give that entry a real backing directory on the
	 * first selected partition so the mount does not fight a read-only root;
	 * the entry is rendered as part of the synthetic root. The host test keeps
	 * its source tree pristine and does not exercise the Windows root helper.
	 */
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];
		char path[VFS_PATH_MAX];
		uint32_t identifier;
		const char *helper = "System Volume Information";

		if (!part->selected || !part->mounted || part->mount[0] == '\0') {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", part->mount, helper);
		(void)vfs_mkdir(path); /* -EEXIST is fine: the directory may already be there */
		identifier = lazy_dir_alloc(helper);
		if (identifier == UINT32_MAX) {
			break;
		}
		lazy.dirs[identifier].parent = part->dir_id;
		lazy.dirs[identifier].partition = partition;
		lazy.root_entries[lazy.root_entry_count].dir_id = identifier;
		lazy.root_entries[lazy.root_entry_count].slot = slot;
		lazy.root_entries[lazy.root_entry_count].entries = lazy_name_entries(helper);
		slot += lazy_name_entries(helper);
		lazy.root_entry_count++;
		break;
	}
#endif
	lazy.dirs[PROXY_LAZY_ROOT_ID].entry_count = slot + 1U;
	return 0;
}

/**
 * @brief Map the probed file system name to a registered VFS type.
 *
 * @details The probe reports the exact generation ("fat32", "ext4") while the
 * VFS registers one type for a family. Mapping here avoids probing every type
 * a second time and the log lines a wrong guess would print.
 *
 * @param[in] info Volume information from the probe.
 * @return A registered type name, or NULL when the type is not known.
 */
static const char *lazy_mount_type(const struct vfs_volume_info *info)
{
	if (strncmp(info->fstype, "ext", 3U) == 0) {
		return "ext";
	}
	if (strcmp(info->fstype, "fat32") == 0) {
		return "fat32";
	}
	if (strcmp(info->fstype, "exfat") == 0) {
		return "exfat";
	}
	if (strncmp(info->fstype, "fat", 3U) == 0) {
		return "fat";
	}
	return NULL;
}

/**
 * @brief Open the source disk for its size and describe the selected partitions.
 *
 * @param[in] source_disk Block device name of the whole source disk.
 * @return 0 on success, or -ENODEV when the disk is not registered.
 */
static int lazy_probe_partitions(const char *source_disk)
{
	struct block_device *source = bdev_open_by_name(source_disk);

	if (source == NULL) {
		return -ENODEV;
	}
	snprintf(lazy.source_name, sizeof(lazy.source_name), "%s", source_disk);
	bdev_close(source);
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];
		struct block_device *bdev;

		if (!part->selected) {
			continue;
		}
		snprintf(part->device, sizeof(part->device), "%sp%u", source_disk, partition + 1U);
		snprintf(part->mount, sizeof(part->mount), "/lazy%u", partition + 1U);
		part->status = -ENODEV;
		bdev = bdev_open_by_name(part->device);
		if (bdev == NULL) {
			continue;
		}
		part->present = true;
		part->start_sector = bdev->start_sect;
		part->sectors = bdev->nr_sects;
		part->type = bdev->part_type;
		bdev_close(bdev);
		part->status = vfs_probe(part->device, &part->volume);
		if (part->status == 0) {
			/* The lazy view writes host changes back to the source. */
			int mounted = vfs_mount_ex(part->device, part->mount,
				lazy_mount_type(&part->volume), 0U);

			part->mounted = mounted == 0;
			if (!part->mounted) {
				part->status = mounted;
			}
		}
	}
	return 0;
}

/**
 * @brief Start the lazy view with an explicit partition selection.
 *
 * @details Startup runs in six stages: describe the selected partitions, size
 * their reservations, allocate the persistent tables, build the system areas,
 * create the partition folders and register the block device. Any failure
 * unmounts the sources and frees everything again.
 *
 * @param[in] source_disk          Whole-disk block device name.
 * @param[in] selected_partitions Bit mask of partitions; bit zero is partition one.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_lazy_start(const char *source_disk, uint32_t selected_partitions)
{
	uint32_t selected_count = 0U;
	uint32_t root_slots;
	int result = 0;

	if (source_disk == NULL || source_disk[0] == '\0' || selected_partitions == 0U ||
	    (selected_partitions & ~((1U << PROXY_LAZY_PARTITIONS) - 1U)) != 0U) {
		return -EINVAL;
	}
	result = mutex_lock_timeout(&lazy.lock, WAIT_FOREVER);
	if (result != 0) {
		return result;
	}
	if (lazy.running || lazy.disk != NULL) {
		mutex_unlock(&lazy.lock);
		return -EBUSY;
	}
	(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
	lazy.callbacks = 0U;
	memset(lazy.partitions, 0, sizeof(lazy.partitions));
	lazy.running = false;
	lazy.truncated = false;
	lazy.last_error = 0;
	lazy.evictions = 0U;
	lazy.sectors_read = 0U;
	lazy.sectors_written = 0U;
	lazy.file_bytes = 0U;
	lazy.file_bytes_written = 0U;
	lazy.clean = true;
	lazy.flush_failed = false;
	lazy.write_fd = -1;
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.write_position = 0U;
	lazy.split_slots = 0U;
	lazy.split_dir = PROXY_LAZY_INVALID_DIR;
	lazy.split_offset = 0U;
	lazy.cached_fd = -1;
	lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	lazy.cached_slot = PROXY_LAZY_INVALID_DIR;
	lazy.cached_position = 0U;
	lazy.cache_entries = 0U;
	lazy.cache_bytes = 0U;
	lazy.lru_head = PROXY_LAZY_INVALID_DIR;
	lazy.lru_tail = PROXY_LAZY_INVALID_DIR;
	lazy.tail_clusters = 0U;
	lazy.upcase = NULL;
	lazy.upcase_bytes = 0U;
	lazy.upcase_clusters = 0U;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		lazy.partitions[partition].selected =
			(selected_partitions & (1U << partition)) != 0U;
		if (lazy.partitions[partition].selected) {
			selected_count++;
		}
	}
	result = lazy_probe_partitions(source_disk);
	if (result != 0) {
		goto fail;
	}
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];

		if (!part->selected) {
			continue;
		}
		result = lazy_compute_reservation(part);
		if (result != 0) {
			goto fail;
		}
		/* The tail is backed by real free space on the sources. */
		{
			uint64_t free_clusters = part->volume.free_bytes / PROXY_LAZY_CLUSTER_SIZE;

			if (free_clusters > CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS) {
				free_clusters = CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS;
			}
			lazy.tail_clusters += (uint32_t)free_clusters;
		}
	}
	if (lazy.tail_clusters > CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS) {
		lazy.tail_clusters = CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS;
	}
	lazy.dir_capacity = CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS;
	lazy.dirs = lazy_calloc(lazy.dir_capacity, sizeof(*lazy.dirs));
	lazy.name_arena = lazy_alloc(CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES);
	lazy.overlay = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX,
		sizeof(*lazy.overlay));
	lazy.overlay_names = lazy_alloc(CONFIG_STORAGE_PROXY_LAZY_OVERLAY_NAME_BYTES);
	lazy.pending = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS,
		sizeof(*lazy.pending));
	lazy.fat_log = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES,
		sizeof(*lazy.fat_log));
	if (lazy.dirs == NULL || lazy.name_arena == NULL || lazy.overlay == NULL ||
	    lazy.overlay_names == NULL || lazy.pending == NULL || lazy.fat_log == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	lazy.dir_count = 0U;
	lazy.name_used = 0U;
	lazy.overlay_count = 0U;
	lazy.overlay_name_used = 0U;
	lazy.pending_count = 0U;
	lazy.fat_count = 0U;
	lazy.range_count = 0U;
	lazy.range_capacity = 0U;
	lazy.ranges = NULL;
	lazy.root_entry_count = 0U;
	{
		uint32_t root_identifier = lazy_dir_alloc("");

		if (root_identifier != PROXY_LAZY_ROOT_ID) {
			result = -ENOMEM;
			goto fail;
		}
		lazy.dirs[root_identifier].is_root = true;
		lazy.dirs[root_identifier].cache_ready = true;
		lazy.dirs[root_identifier].extent_ready = true;
	}
	root_slots = PROXY_LAZY_ROOT_SYSTEM_SLOTS + selected_count * 3U + 1U;
	lazy.root_clusters = (root_slots + PROXY_LAZY_SLOTS_PER_CLUSTER - 1U) /
		PROXY_LAZY_SLOTS_PER_CLUSTER;
	if (lazy.root_clusters == 0U) {
		lazy.root_clusters = 1U;
	}
	/* Size the up-case table first: its cluster count feeds the geometry. */
	lazy.upcase_bytes = 0U;
	lazy.upcase_clusters = 0U;
	result = lazy_compute_geometry();
	if (result != 0) {
		goto fail;
	}
	result = lazy_create_partition_folders();
	if (result != 0) {
		goto fail;
	}
	lazy.disk = alloc_disk();
	if (lazy.disk == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	strcpy(lazy.disk->disk_name, PROXY_LAZY_DISK_NAME);
	lazy.disk->private_data = &lazy;
	lazy.disk->fops = &lazy_disk_operations;
	lazy.disk->capacity = lazy.total_sectors;
	lazy.disk->logical_block_size = PROXY_LAZY_SECTOR_SIZE;
	lazy.disk->flags = 0U;
	(void)__atomic_store_n(&lazy.armed, true, __ATOMIC_SEQ_CST);
	result = add_disk(lazy.disk);
	if (result != 0) {
		(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
		put_disk(lazy.disk);
		lazy.disk = NULL;
		goto fail;
	}
	lazy.running = true;
	lazy.usb_active = false;
	pr_info("lazy proxy: %s, %u clusters, %u reserved, %u tail, %u partitions\n",
		PROXY_LAZY_DISK_NAME, lazy.total_clusters, lazy.tail_first - 2U,
		lazy.tail_clusters, selected_count);
	mutex_unlock(&lazy.lock);
	return 0;
fail:
	(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
	lazy_release();
	mutex_unlock(&lazy.lock);
	return result;
}

/**
 * @brief Stop the lazy view and release everything it holds.
 *
 * @details New callbacks are refused first, then the ones already inside the
 * block layer are given a bounded time to return. Removing the disk and
 * freeing the caches run under the view lock.
 */
void storage_proxy_lazy_stop(void)
{
	int result;

#ifndef STORAGE_PROXY_HOST_TEST
	if (lazy.usb_active) {
		/* Quiesce the host first, exactly like the eager view's stop path. */
		(void)usb_mass_storage_stop();
		lazy.usb_active = false;
	}
#endif
	(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
#ifndef STORAGE_PROXY_HOST_TEST
	for (uint32_t attempt = 0U; attempt < 200U &&
	     __atomic_load_n(&lazy.callbacks, __ATOMIC_SEQ_CST) != 0U; attempt++) {
		msleep(10);
	}
#endif
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result != 0) {
		pr_warn("lazy proxy: stop could not take the view lock: %d\n", result);
		return;
	}
	if (lazy.running) {
		lazy.running = false;
		/* One last flush carries out what the host wrote; the clean flag keeps it idempotent. */
		if (lazy_flush_locked() != 0) {
			pr_warn("lazy proxy: stop flush failed\n");
		}
		if (lazy.disk != NULL) {
			if (del_gendisk(lazy.disk) != 0) {
				del_gendisk_force(lazy.disk);
			}
			put_disk(lazy.disk);
			lazy.disk = NULL;
		}
		lazy_release();
	}
	mutex_unlock(&lazy.lock);
}

/**
 * @brief Report whether the lazy view is registered as a block device.
 *
 * @return True while @c cardview1 exists.
 */
bool storage_proxy_lazy_running(void)
{
	return lazy.running;
}

/**
 * @brief Return the block device name the lazy view is registered under.
 *
 * @return The constant device name of the lazy view.
 */
const char *storage_proxy_lazy_device_name(void)
{
	return PROXY_LAZY_DISK_NAME;
}

/**
 * @brief Return the number of source directories the view has indexed.
 *
 * @return Directory header count, including the root and the partition folders.
 */
size_t storage_proxy_lazy_directory_count(void)
{
	size_t count = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		count = lazy.dir_count;
		mutex_unlock(&lazy.lock);
	}
	return count;
}

/**
 * @brief Return the number of directory entries currently cached.
 *
 * @return Cached entry count.
 */
uint32_t storage_proxy_lazy_cached_entries(void)
{
	uint32_t count = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		count = lazy.cache_entries;
		mutex_unlock(&lazy.lock);
	}
	return count;
}

/**
 * @brief Return the number of bytes the directory entry caches currently hold.
 *
 * @return Cached byte count, including names and cluster order arrays.
 */
uint32_t storage_proxy_lazy_cached_bytes(void)
{
	uint32_t bytes = 0U;

	if (mutex_lock_timeout(&lazy.lock, 0) == 0) {
		bytes = lazy.cache_bytes;
		mutex_unlock(&lazy.lock);
	}
	return bytes;
}

/**
 * @brief Drop every cached directory entry, keeping extents and the overlay.
 *
 * @details The next read reloads each directory from the source readdir result
 * merged with the overlay. Sizes are first enforced so an evicted cache does
 * not leave a source file longer than the entry the host announced.
 */
void storage_proxy_lazy_cache_drop(void)
{
	if (mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS) != 0) {
		return;
	}
	(void)lazy_enforce_sizes_locked();
	while (lazy.lru_head != PROXY_LAZY_INVALID_DIR) {
		lazy_cache_evict(lazy.lru_head);
	}
	mutex_unlock(&lazy.lock);
}

/**
 * @brief Copy the geometry and reservation regions of the running view.
 *
 * @param[out] layout Receives the layout.
 * @return 0 on success, -EINVAL for a NULL argument, -ENODEV when the view is
 *         stopped, or -ETIMEDOUT when the view lock stayed busy.
 */
int storage_proxy_lazy_layout(struct storage_proxy_lazy_layout *layout)
{
	int result;

	if (layout == NULL) {
		return -EINVAL;
	}
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result != 0) {
		return result;
	}
	if (!lazy.running) {
		mutex_unlock(&lazy.lock);
		return -ENODEV;
	}
	memset(layout, 0, sizeof(*layout));
	layout->total_clusters = lazy.total_clusters;
	layout->data_start = lazy.data_start;
	layout->bitmap_cluster = lazy.bitmap_cluster;
	layout->bitmap_bytes = lazy.bitmap_bytes;
	layout->root_cluster = lazy.root_cluster;
	layout->root_clusters = lazy.root_clusters;
	layout->tail_first = lazy.tail_first;
	layout->tail_clusters = lazy.tail_clusters;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		const struct proxy_lazy_partition *part = &lazy.partitions[partition];

		layout->selected[partition] = part->selected;
		layout->reserve_first[partition] = part->reserve_first;
		layout->reserve_clusters[partition] = part->reserve_clusters;
		if (part->selected && part->dir_id < lazy.dir_count &&
		    lazy.dirs[part->dir_id].extent_ready) {
			layout->partition_root_first[partition] = lazy.dirs[part->dir_id].first_cluster;
			layout->partition_root_clusters[partition] = lazy.dirs[part->dir_id].clusters;
		}
	}
	mutex_unlock(&lazy.lock);
	return 0;
}

/* ---- shell command ---- */

#ifdef CONFIG_STORAGE_PROXY_SHELL
/**
 * @brief Build a partition mask from the partitions that exist on a disk.
 *
 * @param[in] source_disk Whole-disk block device name.
 * @return The mask of present partitions, possibly zero.
 */
static uint32_t lazy_default_mask(const char *source_disk)
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
static int command_lazyproxy(int argc, char **argv)
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

		printf("lazy proxy: %s, source %s, dirs %u, ranges %u\n",
			lazy.running ? "running" : "stopped", lazy.source_name,
			lazy.dir_count, lazy.range_count);
		printf("  cache %u entries, %u bytes, %u evictions, truncated %d, last error %d\n",
			lazy.cache_entries, lazy.cache_bytes, lazy.evictions, lazy.truncated ? 1 : 0,
			lazy.last_error);
		printf("  overlay %u records, pending %u sectors, clean %d, flush failed %d\n",
			lazy.overlay_count, lazy.pending_count, lazy.clean ? 1 : 0,
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

/**
 * @brief Initialise the lazy view's instance and its lock.
 *
 * @return 0 on success, or -ENOMEM when the lock cannot be created.
 */
static int lazy_proxy_init(void)
{
	memset(&lazy, 0, sizeof(lazy));
	lazy.cached_fd = -1;
	lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	lazy.cached_slot = PROXY_LAZY_INVALID_DIR;
	lazy.write_fd = -1;
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.split_dir = PROXY_LAZY_INVALID_DIR;
	lazy.lru_head = PROXY_LAZY_INVALID_DIR;
	lazy.lru_tail = PROXY_LAZY_INVALID_DIR;
	lazy.clean = true;
	mutex_init(&lazy.lock);
	return lazy.lock.handle == NULL ? -ENOMEM : 0;
}

/** @brief Register the lazy view initialiser with the init call table. */
INITCALL(3, 21, lazy_proxy_init);
