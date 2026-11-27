// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_cache.c
 * @brief LRU accounting and eviction of directory entry caches.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- LRU cache ---- */

/**
 * @brief Remove one directory from the LRU list.
 *
 * @param[in,out] dir Directory to remove.
 */
void lazy_lru_remove(struct proxy_lazy_dir *dir)
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
void lazy_lru_touch(uint32_t identifier)
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
void lazy_cache_evict(uint32_t identifier)
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
int lazy_cache_make_room(uint32_t bytes, uint32_t entries)
{
	while (lazy.lru_head != PROXY_LAZY_INVALID_DIR &&
	       (lazy.cache_bytes + bytes > lazy.cache_bytes_max ||
		lazy.cache_entries + entries > lazy.cache_entries_max)) {
		lazy_cache_evict(lazy.lru_head);
	}
	if (lazy.cache_bytes + bytes > lazy.cache_bytes_max ||
	    lazy.cache_entries + entries > lazy.cache_entries_max) {
		return -ENOSPC;
	}
	return 0;
}
