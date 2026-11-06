// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A synthesized exFAT volume whose partition folders are backed by mounted
 * source file systems. Metadata is indexed in RAM and writable volumes
 * translate host file operations back to the source VFS.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <init.h>
#ifdef STORAGE_PROXY_HOST_TEST
#include <log.h>
static inline uint64_t ktime_get_us(void)
{
	return 0U;
}
struct mutex {
	void *handle;
};
#define WAIT_FOREVER (-1)
static inline void mutex_init(struct mutex *lock)
{
	lock->handle = lock;
}
static inline int mutex_lock_timeout(struct mutex *lock, int timeout)
{
	(void)lock;
	(void)timeout;
	return 0;
}
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
#define CONFIG_STORAGE_PROXY_MAX_ENTRIES 2048
#endif
#ifndef CONFIG_STORAGE_PROXY_STRING_BYTES
#define CONFIG_STORAGE_PROXY_STRING_BYTES 262144
#endif
#define PROXY_PARTITIONS STORAGE_PROXY_MAX_PARTITIONS
#define PROXY_SECTOR_SIZE 512U
#define PROXY_SECTORS_PER_CLUSTER 64U
#define PROXY_CLUSTER_SIZE (PROXY_SECTOR_SIZE * PROXY_SECTORS_PER_CLUSTER)
#define PROXY_DIRECTORY_CLUSTERS 1U
#define PROXY_BOOT_SECTORS 24U
#define PROXY_FAT_OFFSET 32U
#define PROXY_ROOT_CLUSTER_MIN 2U
#define PROXY_EOC 0xffffffffU
#define PROXY_DISK_NAME "cardview0"
#define PROXY_INVALID_NODE UINT32_MAX
#define PROXY_WRITE_BUFFER (128U * 1024U)
/* Data that arrives before its directory entry waits here; 4 MiB covers a normal copy burst. */
#define PROXY_PENDING_MAX 1024U
/* Data of a file whose directory entry has not arrived is staged on the source card past this much RAM. */
#define PROXY_RUNS 8U
#define PROXY_RUN_GAP 64U
#define PROXY_STAGE_PREFIX ".cs-stage-"

enum proxy_source {
	PROXY_SOURCE_NONE,
	PROXY_SOURCE_VFS,
	PROXY_SOURCE_MEMORY,
	PROXY_SOURCE_VOLATILE,
};

struct proxy_node {
	uint32_t parent;
	uint32_t first_child;
	uint32_t last_child;
	uint32_t next_sibling;
	uint32_t directory_slot;
	uint32_t name_offset;
	uint32_t source_offset;
	uint32_t first_cluster;
	uint32_t cluster_count;
	uint64_t size;
	uint8_t source;
	bool directory;
	bool scanned;
	bool deleted;
	/* The host reused this entry's slots for another object: it no longer appears in the directory. */
	bool retired;
	/* What an object left behind when the host moved it: a deleted entry that keeps the slots a hole. */
	bool tombstone;
};

struct proxy_pending_sector {
	uint32_t cluster;
	uint16_t sector;
	bool used;
	uint8_t data[PROXY_SECTOR_SIZE];
};

struct proxy_partition {
	char device[VFS_BLK_NAME];
	char mount[16];
	struct vfs_volume_info volume;
	uint64_t start_sector;
	uint64_t sectors;
	int status;
	uint8_t type;
	bool present;
	bool mounted;
	bool selected;
	bool writable;
};

/* A contiguous stretch of clusters written before any directory entry owned it, staged in a source file. */
struct proxy_run {
	bool used;
	uint32_t first_cluster;
	uint32_t clusters;
	/* One bit per cluster whose data has been copied to its file in full; only runs up to 2048 clusters are tracked. */
	uint8_t claimed[256];
	uint32_t partition;
	char path[48];
};

#define PROXY_RUN_TRACKED 2048U

struct storage_proxy {
	struct proxy_node *nodes;
	uint32_t *range_nodes;
	char *strings;
	uint8_t *upcase;
	uint8_t *bitmap;
	uint32_t node_count;
	uint32_t range_count;
	uint32_t string_used;
	uint32_t fat_sectors;
	uint32_t data_start;
	uint32_t total_clusters;
	uint32_t bitmap_cluster;
	uint32_t bitmap_clusters;
	uint32_t bitmap_bytes;
	uint32_t upcase_cluster;
	uint32_t upcase_clusters;
	uint32_t upcase_bytes;
	uint32_t upcase_checksum;
	uint32_t root_cluster;
	uint32_t next_cluster;
	uint64_t total_sectors;
	uint64_t source_sectors;
	char source_name[VFS_BLK_NAME];
	uint16_t volume_flags;
	struct proxy_pending_sector *pending;
	/* Open addressing index of pending sectors by (cluster, sector): an index plus one, 0 is empty. */
	uint32_t *pending_index;
	uint32_t pending_count;
	uint32_t pending_capacity;
	uint32_t pending_index_size;
	/* Consecutive sectors of one file wait here, so the card sees one write instead of one per sector. */
	uint8_t write_buffer[PROXY_WRITE_BUFFER] __attribute__((aligned(64)));
	const char *write_path;
	/* The staging file being appended to stays open: no open, seek and close for every batch. */
	int stage_fd;
	const char *stage_path;
	uint64_t stage_position;
	uint8_t fill_buffer[4096];
	struct proxy_run runs[PROXY_RUNS];
	uint64_t write_offset;
	uint32_t write_length;
	struct proxy_partition partitions[PROXY_PARTITIONS];
	struct gendisk *disk;
	struct mutex lock;
	int cached_fd;
	uint32_t cached_node;
	uint64_t cached_position;
	/* The head of a directory entry group cut by a sector edge, until the next sector brings the rest. */
	uint8_t split_head[19U * 32U];
	/* Directory parsing scratch. It lives here, not on the stack: the USB task has a small stack. */
	uint8_t directory_window[2U * PROXY_SECTOR_SIZE + 19U * 32U];
	uint8_t directory_previous[PROXY_SECTOR_SIZE];
	uint32_t split_parent;
	uint64_t split_offset;
	uint32_t split_slots;
	bool running;
	/* The source card is gone: nothing may touch its volumes any more, only be torn down. */
	volatile bool source_lost;
	bool read_only;
	bool truncated;
	bool layout_ready;
};

/* Where the write path spends its time, to tell a slow card from slow bookkeeping. */
struct proxy_timing {
	uint64_t calls;
	uint64_t microseconds;
};

#define PROXY_WINDOW_SLOTS (3U * PROXY_SECTOR_SIZE / 32U)

struct proxy_statistics {
	struct proxy_timing disk_write;
	struct proxy_timing batch_flush;
	struct proxy_timing directory_write;
	struct proxy_timing pending_store;
	struct proxy_timing pending_flush;
	struct proxy_timing sync_entry;
	uint64_t sectors;
	uint64_t file_bytes;
	uint32_t pending_peak;
	uint32_t write_errors;
	uint64_t staged_sectors;
	uint32_t runs_renamed;
	uint32_t runs_copied;
	uint32_t sync_trace;
	uint32_t stage;
	int last_sync_error;
};

/* The single view and its counters, defined in storage_proxy.c. */
extern struct storage_proxy proxy;
extern struct proxy_statistics proxy_stats;
extern const struct block_device_operations proxy_disk_operations;

static inline const char *proxy_string(uint32_t offset)
{
	return proxy.strings + offset;
}

void *proxy_alloc(size_t size);
void proxy_free(void *pointer);
void *proxy_calloc(size_t count, size_t size);

static inline uint64_t proxy_time_begin(void)
{
	return ktime_get_us();
}

static inline void proxy_time_end(struct proxy_timing *timing, uint64_t begin)
{
	timing->calls++;
	timing->microseconds += ktime_get_us() - begin;
}

static inline void put_le16(void *destination, uint16_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = value;
	bytes[1] = value >> 8;
}

static inline void put_le32(void *destination, uint32_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = value;
	bytes[1] = value >> 8;
	bytes[2] = value >> 16;
	bytes[3] = value >> 24;
}

static inline void put_le64(void *destination, uint64_t value)
{
	uint8_t *bytes = destination;

	put_le32(bytes, (uint32_t)value);
	put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

static inline uint16_t get_le16(const void *source)
{
	const uint8_t *bytes = source;

	return bytes[0] | (uint16_t)bytes[1] << 8;
}

static inline uint32_t get_le32(const void *source)
{
	const uint8_t *bytes = source;

	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static inline uint64_t get_le64(const void *source)
{
	const uint8_t *bytes = source;

	return get_le32(bytes) | (uint64_t)get_le32(bytes + 4U) << 32;
}

/* storage_proxy.c */
int storage_proxy_start_config(const struct storage_proxy_config *config);
int storage_proxy_start(const char *source_disk);
void storage_proxy_stop(void);

/* proxy_layout.c */
/* proxy_index.c: a small read-only file whose text is held in the index. */
int proxy_add_message(uint32_t parent, const char *name, const char *message);

int proxy_layout_add_file(void);

/* proxy_index.c */
void proxy_rebuild_ranges(void);
uint32_t proxy_store_string(const char *string);
uint32_t proxy_add_node(uint32_t parent, const char *name, bool directory, enum proxy_source source, const char *source_data, uint64_t size);
int proxy_mount_partition(uint32_t partition, uint32_t *root_node);
int proxy_scan_directory(uint32_t parent);
uint32_t proxy_find_child(uint32_t parent, const char *name);
uint32_t proxy_find_cluster(uint32_t cluster);
uint32_t proxy_identify_entry(uint32_t parent, const char *name, uint32_t first_cluster, uint64_t size);
int proxy_child_path(uint32_t parent, const char *name, char path[VFS_PATH_MAX]);
void proxy_reparent(uint32_t node_index, uint32_t parent);
void proxy_leave_tombstone(uint32_t node_index);
void proxy_adopt_slot(uint32_t node_index, uint32_t slot);

/* proxy_volume.c */
int proxy_allocate_node_clusters(uint32_t index);
int proxy_compute_geometry(void);
int proxy_allocate_clusters(void);
uint32_t proxy_cluster_node(uint32_t cluster);
uint32_t proxy_fat_entry(uint32_t cluster);
void proxy_boot_record(uint32_t record, uint8_t sector[PROXY_SECTOR_SIZE]);
uint32_t proxy_boot_checksum(void);
void proxy_bitmap_freed(uint64_t byte_offset, const uint8_t *incoming, size_t length);

/* proxy_directory.c */
uint32_t proxy_object_entries(const struct proxy_node *node);
int proxy_read_directory(uint32_t node_index, uint64_t offset, uint8_t sector[PROXY_SECTOR_SIZE]);
int proxy_write_directory(uint32_t parent, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE]);

/* proxy_io.c */
void proxy_drop_read_cache(void);
int proxy_fill_to(int descriptor, uint64_t position);

/* proxy_write.c */
void proxy_stage_close(void);
int proxy_write_flush(void);
int proxy_write_node(uint32_t node_index, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE]);
void proxy_pending_drop(uint32_t cluster, uint16_t sector);
int proxy_orphan_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE]);
void proxy_run_release(struct proxy_run *run);
int proxy_set_extent(uint32_t node_index, uint32_t first_cluster, uint64_t size);
uint32_t proxy_add_unallocated(uint32_t parent, const char *name, bool directory, enum proxy_source source, const char *source_path, uint64_t size);
int proxy_apply_deletes(void);
