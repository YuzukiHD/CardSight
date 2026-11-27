// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_overlay.c
 * @brief Persistent overlay of host-changed directory entries.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- persistent host-change overlay ---- */

/**
 * @brief Store an overlay name in its arena.
 *
 * @param[in] name NUL-terminated name.
 * @return Offset in the arena, or UINT32_MAX when it is full.
 */
uint32_t lazy_overlay_name_store(const char *name)
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
struct proxy_lazy_overlay *lazy_overlay_find_name(uint32_t dir_id, const char *name)
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
uint32_t lazy_overlay_dir_count(uint32_t dir_id)
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
int lazy_overlay_put(uint32_t dir_id, const char *name, uint32_t slot,
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
int lazy_overlay_tombstone(uint32_t dir_id, const char *name, uint32_t slot,
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
void lazy_overlay_clear_dir(uint32_t dir_id)
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
