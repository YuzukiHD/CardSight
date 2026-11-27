// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_private.h
 * @brief Cross-module interfaces of the lazy exFAT proxy engine.
 *
 * @details Every source file of the engine includes this header. The shared
 * constants, types and the view instance are declared in @c lazy_types.h; the
 * prototypes below are grouped by the capability that implements them.
 */
#ifndef CARDSIGHT_STORAGE_LAZY_PRIVATE_H
#define CARDSIGHT_STORAGE_LAZY_PRIVATE_H

#include <errno.h>
#include <stdarg.h>
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

#include "lazy_types.h"

/* Defined by lazy_io.c. */
extern const struct block_device_operations lazy_disk_operations;

#ifdef CONFIG_STORAGE_PROXY_SHELL
int command_lazyproxy(int argc, char **argv);
#endif

/* ---- public query, test and layout accessors of the lazy view ---- */
bool storage_proxy_lazy_running(void);
const char *storage_proxy_lazy_device_name(void);
size_t storage_proxy_lazy_directory_count(void);
uint32_t storage_proxy_lazy_cached_entries(void);
uint32_t storage_proxy_lazy_cached_bytes(void);
uint32_t storage_proxy_lazy_cache_evictions(void);
bool storage_proxy_lazy_truncated(void);
void storage_proxy_lazy_set_progress(void (*callback)(void *context, uint32_t entries,
	const char *path), void *context);
void storage_proxy_lazy_cancel(void);
#ifdef STORAGE_PROXY_HOST_TEST
void storage_proxy_lazy_test_limits(uint32_t cache_entries, uint32_t cache_bytes,
	uint32_t dir_max_entries);
#endif
#ifdef STORAGE_PROXY_HOST_TEST
void storage_proxy_lazy_test_stats(struct storage_proxy_lazy_test_stats *stats);
#endif
void storage_proxy_lazy_cache_drop(void);
int storage_proxy_lazy_layout(struct storage_proxy_lazy_layout *layout);

/* ---- lru accounting and eviction of directory entry caches ---- */
void lazy_lru_remove(struct proxy_lazy_dir *dir);
void lazy_lru_touch(uint32_t identifier);
void lazy_cache_evict(uint32_t identifier);
int lazy_cache_make_room(uint32_t bytes, uint32_t entries);

/* ---- allocation, byte-order, checksum and callback guards shared by the lazy view ---- */
void *lazy_alloc(size_t size);
void lazy_free(void *pointer);
void *lazy_calloc(size_t count, size_t size);
void lazy_put_le16(void *destination, uint16_t value);
void lazy_put_le32(void *destination, uint32_t value);
void lazy_put_le64(void *destination, uint64_t value);
uint32_t lazy_checksum32(uint32_t checksum, uint8_t value);
bool lazy_armed_enter(void);
void lazy_armed_leave(void);
bool lazy_cancel_requested(void);
void lazy_progress_report(uint32_t entries, const char *path);
void lazy_limits_default(void);

/* ---- persistent directory headers, paths, child lists, counting and extent reservation ---- */
int lazy_dir_path(uint32_t identifier, char *path, size_t size);
int lazy_entry_path(uint32_t dir_id, const struct proxy_lazy_dirent *item,
	char *path, size_t size);
uint32_t lazy_dir_alloc(const char *name);
int lazy_subdir_reserve(struct proxy_lazy_dir *dir);
int lazy_subdir_append(struct proxy_lazy_dir *parent, uint32_t child);
int lazy_dir_count(struct proxy_lazy_dir *dir, uint32_t *entries, uint32_t *slots,
	uint32_t *name_bytes);
int lazy_dir_reserve_extent(uint32_t identifier, uint64_t alloc_size);

/* ---- entry slots, sorted caches, lookups and file-data range rebuilds ---- */
int lazy_slot_compare(const void *left, const void *right);
uint32_t lazy_dir_find_child(const struct proxy_lazy_dir *parent, const char *name);
int lazy_dir_child(struct proxy_lazy_dir *parent, const char *name, uint64_t alloc_size,
	uint32_t *child);
int lazy_dir_name_copy(struct proxy_lazy_dir *dir, const char *name, uint32_t *offset);
int lazy_dir_reserve_entry(struct proxy_lazy_dir *dir);
void lazy_dir_sort(struct proxy_lazy_dir *dir);
uint32_t lazy_dir_find_name(const struct proxy_lazy_dir *dir, const char *name,
	bool include_deleted);
uint32_t lazy_dir_find_cluster(const struct proxy_lazy_dir *dir, uint32_t cluster,
	bool include_deleted);
void lazy_dir_clear_deleted(struct proxy_lazy_dir *dir, uint32_t slot, uint32_t entries);
int lazy_dir_insert(struct proxy_lazy_dir *dir, const char *name, bool directory,
	uint32_t slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool host_owned, bool fat_chain, uint32_t *index);
int lazy_add_file_ranges(uint32_t dir_id, uint32_t slot, uint32_t first,
	uint32_t expected, bool fat_chain);
int lazy_rebuild_data_ranges(uint32_t dir_id);
uint32_t lazy_free_slot(uint32_t dir_id, const struct proxy_lazy_dir *dir,
	uint32_t count, uint32_t group, uint32_t slot);

/* ---- first-load indexing of one source directory ---- */
int lazy_dir_load(uint32_t identifier);

/* ---- block-device write/flush callbacks, pending deletes and size enforcement ---- */
int lazy_apply_deletes_locked(void);
int lazy_enforce_sizes_locked(void);
int lazy_flush_locked(void);
uint32_t lazy_write_data_run(uint64_t sector, const uint8_t *input, uint32_t count,
	int *result);
int lazy_disk_write(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count);
int lazy_disk_flush(struct gendisk *disk);
void lazy_disk_free(struct gendisk *disk);

/* ---- start, stop, probe, release and root construction of the view ---- */
void lazy_release(void);
void lazy_create_root_files(uint32_t *slot);
int lazy_create_partition_folders(void);
const char *lazy_mount_type(const struct vfs_volume_info *info);
int lazy_probe_partitions(const char *source_disk);
int storage_proxy_lazy_start(const char *source_disk, uint32_t selected_partitions);
void storage_proxy_lazy_stop(void);
int lazy_proxy_init(void);

/* ---- name arena, utf-16 name conversion, name hashing and the up-case table ---- */
uint32_t lazy_store_name(const char *name);
const char *lazy_dir_name(uint32_t identifier);
size_t lazy_utf16(const char *input, uint16_t *output, size_t capacity);
uint32_t lazy_name_entries(const char *name);
uint16_t lazy_name_hash(const uint16_t *name, size_t length);
int lazy_build_upcase(void);
size_t lazy_utf8(const uint16_t *input, size_t length, char *output, size_t capacity);
bool lazy_entry_name(const uint8_t *slots, uint32_t slot, uint32_t secondaries,
	char name[VFS_NAME_MAX + 1]);

/* ---- persistent overlay of host-changed directory entries ---- */
uint32_t lazy_overlay_name_store(const char *name);
struct proxy_lazy_overlay *lazy_overlay_find_name(uint32_t dir_id, const char *name);
uint32_t lazy_overlay_dir_count(uint32_t dir_id);
int lazy_overlay_put(uint32_t dir_id, const char *name, uint32_t slot,
	uint32_t first_cluster, uint32_t clusters, uint64_t size, bool directory, bool fat_chain);
int lazy_overlay_tombstone(uint32_t dir_id, const char *name, uint32_t slot,
	bool directory, uint16_t entries);
void lazy_overlay_clear_dir(uint32_t dir_id);

/* ---- ram pending table for host data and orphan handling ---- */
int lazy_pending_store(uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE]);
void lazy_pending_drop_cluster(uint32_t cluster);
void lazy_pending_compact(void);
bool lazy_pending_has_cluster(uint32_t cluster);
int lazy_orphan_store(uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE]);
void lazy_forget_freed_cluster(uint32_t cluster);

/* ---- sorted view-cluster range index of directory extents and file data ---- */
int lazy_range_grow(void);
int lazy_range_add(uint32_t base, uint32_t clusters, uint32_t dir_id, uint32_t kind,
	uint32_t slot, uint32_t offset);
void lazy_range_remove_owner(uint32_t dir_id, uint32_t kind);
void lazy_range_remove_data_cluster(uint32_t cluster);
int lazy_range_find(uint32_t cluster, struct proxy_lazy_range *found);

/* ---- file reads, cached descriptors and sector reads of the view ---- */
void lazy_write_cache_drop(void);
int lazy_read_file(uint32_t dir_id, uint32_t entry_index, uint64_t offset,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE]);
int lazy_read_sector(uint64_t sector_number, uint8_t sector[PROXY_LAZY_SECTOR_SIZE]);
int lazy_disk_read(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count);

/* ---- exfat entry encoding and the synthesized root reports ---- */
void lazy_entry_raw(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain, uint32_t position, uint8_t entry[32]);
uint16_t lazy_entry_checksum(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain);
void lazy_render_entry(const char *name, bool directory, uint32_t first_cluster,
	uint32_t clusters, uint64_t size, bool fat_chain, uint32_t position, uint8_t entry[32]);
void lazy_root_system_entry(uint32_t position, uint8_t entry[32]);
uint32_t lazy_entry_by_slot(const struct proxy_lazy_dir *dir, uint32_t wanted);
const char *lazy_root_file_name(uint32_t kind);
int lazy_render_root_slot(uint32_t slot, uint8_t entry[32]);
int lazy_render_directory(uint32_t identifier, uint64_t offset,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE]);
size_t lazy_text_add(char *buffer, size_t capacity, size_t used,
	const char *format, ...);
uint32_t lazy_root_text(uint32_t kind, char *buffer, size_t capacity);
int lazy_root_text_read(uint32_t cluster, uint32_t sector_in_cluster,
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE]);
bool lazy_root_text_contains(uint32_t cluster);

/* ---- sparse staging files for host data that outlives the pending table ---- */
bool lazy_run_bit(const uint8_t *bits, uint32_t index);
void lazy_run_set(uint8_t *bits, uint32_t index);
void lazy_run_clear(uint8_t *bits, uint32_t index);
struct proxy_lazy_run *lazy_run_find(uint32_t cluster);
struct proxy_lazy_run *lazy_run_extendable(uint32_t cluster);
void lazy_run_release(struct proxy_lazy_run *run);
void lazy_run_release_all(void);
struct proxy_lazy_run *lazy_run_create(uint32_t first);
int lazy_run_write(struct proxy_lazy_run *run, uint32_t cluster, uint16_t sector,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE]);
int lazy_run_read(struct proxy_lazy_run *run, uint64_t offset,
	uint8_t data[PROXY_LAZY_SECTOR_SIZE]);
void lazy_run_drop_cluster(uint32_t cluster);

/* ---- view geometry, reservation allocation, boot region, fat and system chains ---- */
int lazy_alloc_clusters(uint32_t partition, uint32_t clusters, uint32_t *first, uint32_t *count);
uint32_t lazy_fat_next(uint32_t cluster);
int lazy_fat_set(uint32_t cluster, uint32_t next);
int lazy_fat_write(uint32_t fat_sector, const uint8_t data[PROXY_LAZY_SECTOR_SIZE]);
int lazy_compute_reservation(struct proxy_lazy_partition *part);
int lazy_compute_geometry(void);
void lazy_boot_record(uint32_t record, uint8_t sector[PROXY_LAZY_SECTOR_SIZE]);
uint32_t lazy_boot_checksum(void);
uint32_t lazy_system_chain(uint32_t cluster, uint32_t first, uint32_t count);
uint32_t lazy_fat_entry(uint32_t cluster);

/* ---- file data writes and claiming staged clusters ---- */
int lazy_fill_to(int descriptor, uint64_t position);
int lazy_write_bytes(uint32_t dir_id, uint32_t entry_index, uint64_t offset,
	const uint8_t *data, size_t length);
int lazy_claim_pending_dir(uint32_t dir_id);
int lazy_claim_runs(void);

/* ---- directory sector parsing, split-group window and sector writes ---- */
uint16_t lazy_get_le16(const uint8_t *bytes);
uint32_t lazy_get_le32(const uint8_t *bytes);
uint64_t lazy_get_le64(const uint8_t *bytes);
int lazy_write_directory(uint32_t dir_id, uint64_t offset,
	const uint8_t data[PROXY_LAZY_SECTOR_SIZE]);
uint32_t lazy_directory_extension(uint32_t cluster);
int lazy_map_directory_cluster(uint32_t dir_id, uint32_t cluster);
int lazy_write_sector(uint64_t sector_number, const uint8_t data[PROXY_LAZY_SECTOR_SIZE]);

/* ---- host create, rename, delete, resize and synchronisation ---- */
int lazy_child_path(uint32_t dir_id, const char *name, char *path, size_t size);
int lazy_source_create(uint32_t dir_id, const char *name, bool directory);
int lazy_truncate_entry(uint32_t dir_id, uint32_t entry_index, uint64_t size);
int lazy_entry_resize(uint32_t dir_id, uint32_t entry_index, uint64_t size);
void lazy_dir_parent_chained(uint32_t dir_id);
int lazy_dir_extend(uint32_t dir_id, uint32_t wanted);
int lazy_update_entry(uint32_t dir_id, uint32_t entry_index, bool directory,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain);
void lazy_source_remove(uint32_t dir_id, const char *name, bool directory);
int lazy_create_entry(uint32_t dir_id, const char *name, bool directory,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain);
void lazy_overlay_forget(uint32_t dir_id, const char *name);
int lazy_rename_entry(uint32_t dir_id, uint32_t entry_index, const char *name,
	uint32_t host_slot, uint32_t first_cluster, uint32_t clusters, uint64_t size,
	bool fat_chain);
int lazy_delete_entry(uint32_t dir_id, uint32_t slot);
int lazy_sync_entry(uint32_t dir_id, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, bool fat_chain, uint32_t host_slot);

#endif /* CARDSIGHT_STORAGE_LAZY_PRIVATE_H */
