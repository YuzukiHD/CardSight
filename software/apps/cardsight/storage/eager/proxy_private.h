// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_private.h
 * @brief Cross-module interfaces of the eager exFAT proxy engine.
 *
 * @details Every source file of the engine includes this header after the
 * shared state in @c proxy_types.h. Prototypes are grouped by the module that
 * implements them.
 */
#ifndef CARDSIGHT_STORAGE_EAGER_PROXY_PRIVATE_H
#define CARDSIGHT_STORAGE_EAGER_PROXY_PRIVATE_H

#include <bits.h>
#include <size.h>

#include "proxy_types.h"


/* proxy_core.c, proxy_lifecycle.c and proxy_query.c */
/**
 * @brief Start the view with an explicit partition selection.
 *
 * @param[in] config Source disk and selection mask.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_start_config(const struct storage_proxy_config *config);
/**
 * @brief Start the view over every partition found on the disk.
 *
 * @param[in] source_disk Block device name of the whole disk.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_start(const char *source_disk);
/**
 * @brief Stop the view and release everything it holds.
 */
void storage_proxy_stop(void);

/* proxy_layout.c */
/* proxy_index.c and proxy_scan.c: a small read-only file whose text is held in the index. */
/**
 * @brief Add a read-only text file to a directory of the view.
 *
 * @param[in] parent  Node index of the containing directory.
 * @param[in] name    Name of the file inside the view.
 * @param[in] message Text the file returns when read.
 * @return 0 on success, or -ENOMEM.
 */
int proxy_add_message(uint32_t parent, const char *name, const char *message);

/**
 * @brief Add the _LAYOUT.TXT file that describes the source disk.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_layout_add_file(void);

/* proxy_index.c */
/**
 * @brief Rebuild the cluster-sorted node index used by @c proxy_cluster_node().
 */
void proxy_rebuild_ranges(void);
/**
 * @brief Copy a string into the arena and return its offset.
 *
 * @param[in] string String to copy.
 * @return Offset in the arena, or UINT32_MAX when it does not fit.
 */
uint32_t proxy_store_string(const char *string);
/**
 * @brief Add one node to the index.
 *
 * @param[in] parent      Node index of the containing directory.
 * @param[in] name        Requested name; made unique and sanitised.
 * @param[in] directory   True for a directory.
 * @param[in] source      Where the node's data comes from.
 * @param[in] source_data Source path or text, or NULL.
 * @param[in] size        File size in bytes.
 * @return The new node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_add_node(uint32_t parent, const char *name, bool directory, enum proxy_source source, const char *source_data, uint64_t size);
/**
 * @brief Mount one source partition and add its folder to the root.
 *
 * @param[in]  partition Zero-based partition index.
 * @param[out] root_node Node index of the folder that was added.
 * @return 0 on success, or a negative errno.
 */
int proxy_mount_partition(uint32_t partition, uint32_t *root_node);
/**
 * @brief Index the children of a source directory, once.
 *
 * @param[in] parent Node index of the directory.
 * @return 0 on success, or a negative errno.
 */
int proxy_scan_directory(uint32_t parent);
/**
 * @brief Find a live child by name.
 *
 * @param[in] parent Parent node index.
 * @param[in] name   Name to look for, compared case-insensitively.
 * @return The child index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_find_child(uint32_t parent, const char *name);
/**
 * @brief Find a live node by its first cluster.
 *
 * @param[in] cluster First cluster to look for.
 * @return The node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_find_cluster(uint32_t cluster);
/**
 * @brief Match a host directory entry to a known node, or report that it is new.
 *
 * @param[in] parent        Parent node index.
 * @param[in] name          Name from the host entry.
 * @param[in] first_cluster First cluster from the host entry.
 * @param[in] size          Data length from the host entry.
 * @return The matching node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_identify_entry(uint32_t parent, const char *name, uint32_t first_cluster, uint64_t size);
/**
 * @brief Build the source path of a child of a source-backed directory.
 *
 * @param[in]  parent Parent node index.
 * @param[in]  name   Child name.
 * @param[out] path   Buffer receiving the path.
 * @return 0 on success, -ENODEV when the parent has no source, or -ENAMETOOLONG.
 */
int proxy_child_path(uint32_t parent, const char *name, char path[VFS_PATH_MAX]);
/**
 * @brief Move a node under a new parent and give it a fresh directory slot.
 *
 * @param[in] node_index Node to move.
 * @param[in] parent     New parent node index.
 */
void proxy_reparent(uint32_t node_index, uint32_t parent);
/**
 * @brief Leave a deleted placeholder that keeps the slots of a moved node.
 *
 * @param[in] node_index Node whose slots must stay occupied.
 */
void proxy_leave_tombstone(uint32_t node_index);
/**
 * @brief Take the host's directory slot and retire whatever held it before.
 *
 * @param[in] node_index Node taking the slot.
 * @param[in] slot       Absolute first slot of the host entry.
 */
void proxy_adopt_slot(uint32_t node_index, uint32_t slot);

/* proxy_volume.c */
/**
 * @brief Give one node its clusters from the unallocated tail.
 *
 * @param[in] index Node index to allocate for.
 * @return 0 on success, or -ENOSPC.
 */
int proxy_allocate_node_clusters(uint32_t index);
/**
 * @brief Size the view and place its FAT, bitmap, up-case table and root cluster.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_compute_geometry(void);
/**
 * @brief Hand clusters to every node before a host reads the allocation bitmap.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_allocate_clusters(void);
/**
 * @brief Find the node that owns a cluster.
 *
 * @param[in] cluster Cluster number.
 * @return The node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_cluster_node(uint32_t cluster);
/**
 * @brief Return the FAT entry for a cluster.
 *
 * @param[in] cluster Cluster number.
 * @return The chain value stored in the FAT.
 */
uint32_t proxy_fat_entry(uint32_t cluster);
/**
 * @brief Build one sector of the exFAT boot region.
 *
 * @param[in]  record Boot record number, 0 through 11.
 * @param[out] sector Sector buffer to fill.
 */
void proxy_boot_record(uint32_t record, uint8_t sector[PROXY_SECTOR_SIZE]);
/**
 * @brief Compute the checksum of the first eleven boot records.
 *
 * @return The boot region checksum.
 */
uint32_t proxy_boot_checksum(void);
/**
 * @brief Drop cached data for clusters the host just marked free.
 *
 * @param[in] byte_offset Offset of the bitmap byte the write starts at.
 * @param[in] incoming    Bytes the host wrote.
 * @param[in] length      Number of bytes to examine.
 */
void proxy_bitmap_freed(uint64_t byte_offset, const uint8_t *incoming, size_t length);

/* proxy_dir.c and proxy_dir_write.c */
/**
 * @brief Number of 32-byte directory entries a node occupies.
 *
 * @param[in] node Node to measure.
 * @return Entry count, including the stream and name entries.
 */
uint32_t proxy_object_entries(const struct proxy_node *node);
/**
 * @brief Render one sector of a directory node.
 *
 * @param[in]  node_index Node index of the directory.
 * @param[in]  offset     Byte offset of the sector inside the directory.
 * @param[out] sector     Buffer receiving the sector.
 * @return 0 on success, or a negative errno.
 */
int proxy_read_directory(uint32_t node_index, uint64_t offset, uint8_t sector[PROXY_SECTOR_SIZE]);
/**
 * @brief Parse one directory sector the host wrote and sync the entries it holds.
 *
 * @param[in] parent Node index of the directory.
 * @param[in] offset Byte offset of the sector inside the directory.
 * @param[in] data   New sector contents.
 * @return 0 on success, or a negative errno.
 */
int proxy_write_directory(uint32_t parent, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE]);

/* proxy_io.c */
/**
 * @brief Forget the cached read descriptor and position.
 */
void proxy_drop_read_cache(void);
/**
 * @brief Fill a file with zeros up to a position before writing there.
 *
 * @param[in] descriptor Open descriptor of the file.
 * @param[in] position   Position the following write will use.
 * @return 0 on success, or a negative errno.
 */
int proxy_fill_to(int descriptor, uint64_t position);

/* proxy_write.c, proxy_pending.c and proxy_staging.c */
/**
 * @brief Close the staging file kept open for appends.
 */
void proxy_stage_close(void);
/**
 * @brief Write the batched sectors to their file.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_write_flush(void);
/**
 * @brief Queue one sector of a file for write-back.
 *
 * @param[in] node_index Node the sector belongs to.
 * @param[in] offset     Byte offset inside the file.
 * @param[in] data       Sector contents.
 * @return 0 on success, or a negative errno.
 */
int proxy_write_node(uint32_t node_index, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE]);
/**
 * @brief Forget the data held for one sector.
 *
 * @param[in] cluster Cluster the sector belongs to.
 * @param[in] sector  Sector offset inside the cluster.
 */
void proxy_pending_drop(uint32_t cluster, uint16_t sector);
/**
 * @brief Store a sector that has no directory entry yet.
 *
 * @param[in] cluster Cluster the sector belongs to.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, or a negative errno.
 */
int proxy_orphan_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE]);
/**
 * @brief Release a staging run and delete its staging file.
 *
 * @param[in] run Run to release.
 */
void proxy_run_release(struct proxy_run *run);
/**
 * @brief Adopt the extent a host entry described for a node.
 *
 * @param[in] node_index    Node index.
 * @param[in] first_cluster First cluster claimed by the entry.
 * @param[in] size          Data length claimed by the entry.
 * @return 0 on success, or a negative errno.
 */
int proxy_set_extent(uint32_t node_index, uint32_t first_cluster, uint64_t size);
/**
 * @brief Add a node without giving it clusters yet.
 *
 * @param[in] parent      Parent node index.
 * @param[in] name        Requested name.
 * @param[in] directory   True for a directory.
 * @param[in] source      Where the node's data comes from.
 * @param[in] source_path Source path, or NULL.
 * @param[in] size        File size in bytes.
 * @return The new node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_add_unallocated(uint32_t parent, const char *name, bool directory, enum proxy_source source, const char *source_path, uint64_t size);
/**
 * @brief Carry out the deletes the host requested and sync the source mounts.
 *
 * @return 0 on success, or a negative errno.
 */
int proxy_apply_deletes(void);

/* ---- helpers shared between the split modules ---- */
void proxy_unmount_sources(void);
void proxy_release(void);
int storage_proxy_init(void);
int proxy_range_compare(const void *left, const void *right);
void proxy_sanitize_name(const char *input, char *output, size_t size);
bool proxy_name_exists(uint32_t parent, const char *name);
void proxy_unique_name(uint32_t parent, const char *input, char *output, size_t size);
uint32_t proxy_entry_span_name(const char *name);
uint32_t proxy_next_directory_slot(uint32_t parent);
uint32_t proxy_find_live_cluster(uint32_t cluster);
uint32_t proxy_find_live_child(uint32_t parent, const char *name);
size_t proxy_utf16(const char *input, uint16_t *output, size_t capacity);
uint16_t proxy_name_hash(const uint16_t *name, size_t length);
void proxy_object_entry_raw(const struct proxy_node *node, uint32_t position,
				   uint8_t entry[32]);
uint16_t proxy_object_checksum(const struct proxy_node *node);
void proxy_object_entry(const struct proxy_node *node, uint32_t position,
			       uint8_t entry[32]);
void proxy_root_system_entry(uint32_t position, uint8_t entry[32]);
bool proxy_directory_entry(const struct proxy_node *directory,
				  uint32_t wanted, uint8_t entry[32]);
size_t proxy_utf8(const uint16_t *input, size_t length, char *output, size_t capacity);
int proxy_sync_entry_inner(uint32_t parent, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, uint32_t host_slot);
int proxy_sync_entry(uint32_t parent, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, uint32_t host_slot);
bool proxy_entry_name(const uint8_t *sector, uint32_t slot, uint32_t secondaries,
	char name[VFS_NAME_MAX + 1]);
void proxy_mark_removed(uint32_t parent, const uint8_t *sector, uint32_t slot, uint32_t absolute_slot);
int proxy_write_directory_inner(uint32_t parent, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE]);
bool proxy_is_stage_path(const char *path);
int proxy_batch_write(const char *path, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE]);
uint32_t proxy_pending_hash(uint32_t cluster, uint16_t sector);
int proxy_pending_reindex(uint32_t capacity);
int proxy_pending_store_inner(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE]);
int proxy_pending_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE]);
int proxy_pending_flush_inner(uint32_t node_index);
int proxy_pending_flush(uint32_t node_index);
uint32_t proxy_path_partition(const char *path);
bool proxy_pending_has_cluster(uint32_t cluster);
struct proxy_run *proxy_find_run(uint32_t cluster);
uint32_t proxy_run_unclaimed(const struct proxy_run *run);
struct proxy_run *proxy_new_run(uint32_t cluster);
int proxy_migrate_pending(struct proxy_run *run, uint32_t start, uint32_t end);
bool proxy_run_claimed(const struct proxy_run *run, uint32_t cluster);
bool proxy_run_all_claimed(const struct proxy_run *run);
int proxy_copy_run_range(struct proxy_run *run, const struct proxy_node *node, uint32_t low, uint32_t high);
int proxy_claim_runs(uint32_t node_index);

#endif /* CARDSIGHT_STORAGE_EAGER_PROXY_PRIVATE_H */
