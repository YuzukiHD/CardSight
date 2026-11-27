// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_types.h
 * @brief Constants, types and shared state of the lazy exFAT proxy engine.
 */
#ifndef CARDSIGHT_STORAGE_LAZY_TYPES_H
#define CARDSIGHT_STORAGE_LAZY_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "storage/proxy_lazy.h"

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
/** @brief Sectors-per-cluster shift of the synthesized volume (4 KiB clusters). */
#define PROXY_LAZY_SECTORS_PER_CLUSTER_SHIFT 3U
/** @brief Sectors per cluster of the synthesized volume. */
#define PROXY_LAZY_SECTORS_PER_CLUSTER (1U << PROXY_LAZY_SECTORS_PER_CLUSTER_SHIFT)
/** @brief Cluster size of the view in bytes (4 KiB). */
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
/** @brief Entries between two cancellation checks and progress reports. */
#define PROXY_LAZY_PROGRESS_INTERVAL 64U
/** @brief Number of synthesized informational files in the root directory. */
#define PROXY_LAZY_ROOT_FILES 2U
/** @brief Size of each synthesized root text file, in bytes (one sector). */
#define PROXY_LAZY_ROOT_TEXT_BYTES (PROXY_LAZY_SECTOR_SIZE)

/**
 * @brief Kinds of entry the synthesized root directory renders.
 */
enum proxy_lazy_root_kind {
	/** @brief A partition folder backed by a source directory. */
	PROXY_LAZY_ROOT_FOLDER,
	/** @brief The synthesized _LAYOUT.TXT report. */
	PROXY_LAZY_ROOT_LAYOUT,
	/** @brief The synthesized _DIR_LIMIT.TXT report. */
	PROXY_LAZY_ROOT_LIMITS,
};

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
/** @brief Largest free tail the host is shown, in 4 KiB clusters (32 GiB). */
#define CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS 8388608U
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
/** @brief Sectors of host data that may wait in RAM for their directory entry. */
#define CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS 1024U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_RUNS
/** @brief Staging files that may hold host data waiting for its entry. */
#define CONFIG_STORAGE_PROXY_LAZY_RUNS 4U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS
/** @brief Largest span one staging file maps, in view clusters (128 MiB). */
#define CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS 32768U
#endif
#ifndef CONFIG_STORAGE_PROXY_LAZY_RUN_GAP
/** @brief Clusters a staging run may skip when the host allocates ahead. */
#define CONFIG_STORAGE_PROXY_LAZY_RUN_GAP 256U
#endif
/** @brief Prefix of a staging file, placed in the source partition root. */
#define PROXY_LAZY_STAGE_PREFIX ".cs-lazy-stage-"
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
 * @brief One entry of the view root: a partition folder or a synthesized file.
 */
struct proxy_lazy_root_entry {
	/** @brief Directory identifier of the folder; unused for a file. */
	uint32_t dir_id;
	/** @brief Absolute first slot of the entry group. */
	uint32_t slot;
	/** @brief Slots the entry group occupies. */
	uint32_t entries;
	/** @brief First cluster of a synthesized file's data, or zero. */
	uint32_t first_cluster;
	/** @brief Number of clusters of a synthesized file's data. */
	uint32_t clusters;
	/** @brief One of @ref proxy_lazy_root_kind. */
	uint32_t kind;
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
 * @brief A staging file that holds host data until its directory entry arrives.
 *
 * @details Small bursts wait in the RAM pending table. Beyond that limit the
 * data is written to a sparse file in the source partition root at the offset
 * @c (cluster - first_cluster) * 4 KiB + sector * 512, so a stream of hundreds
 * of megabytes needs card space rather than heap. @c written marks the clusters
 * that actually hold data; empty gaps are never copied into a file. The run is
 * released, and its file unlinked, once every written cluster has been claimed.
 */
struct proxy_lazy_run {
	/** @brief True while the slot holds a staging file. */
	bool used;
	/** @brief Open descriptor, or -1. */
	int fd;
	/** @brief Partition the staging file lives on. */
	uint32_t partition;
	/** @brief First view cluster the run maps. */
	uint32_t first_cluster;
	/** @brief Number of clusters the run spans. */
	uint32_t clusters;
	/** @brief Clusters in the span that hold staged data. */
	uint8_t written[CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS / 8U];
	/** @brief Clusters already claimed by a directory entry. */
	uint8_t claimed[CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS / 8U];
	/** @brief Clusters the host freed; their staged bytes are stale. */
	uint8_t dropped[CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS / 8U];
	/** @brief Position of @c fd, so a sequential stream need not seek. */
	uint64_t position;
	/** @brief Source path of the staging file. */
	char path[VFS_PATH_MAX];
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
	struct proxy_lazy_root_entry root_entries[PROXY_LAZY_PARTITIONS + PROXY_LAZY_ROOT_FILES + 1U];
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
	/** @brief Staging files for data the RAM table could not hold. */
	struct proxy_lazy_run *runs;
	/** @brief Host sectors that went to a staging file. */
	uint64_t staged_sectors;
	/** @brief Set while a run claim is walking ranges. */
	bool claiming;
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
	/** @brief Entries all LRU caches may hold together. */
	uint32_t cache_entries_max;
	/** @brief Bytes all LRU caches may hold together. */
	uint32_t cache_bytes_max;
	/** @brief Entries one source directory may expose before it is truncated. */
	uint32_t dir_max_entries;
	/** @brief Set to abort a running directory count or load. */
	volatile bool cancel;
	/** @brief Directory indexing progress callback, or NULL. */
	void (*progress_callback)(void *context, uint32_t entries, const char *path);
	/** @brief Opaque pointer passed to @c progress_callback. */
	void *progress_context;
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

/** @brief The one lazy view instance, defined in @c lazy_lifecycle.c. */
extern struct proxy_lazy lazy;

#endif /* CARDSIGHT_STORAGE_LAZY_TYPES_H */
