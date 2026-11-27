// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_range.c
 * @brief Sorted view-cluster range index of directory extents and file data.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- range index ---- */

/**
 * @brief Grow the sorted range index.
 *
 * @return 0 on success, or -ENOMEM.
 */
int lazy_range_grow(void)
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
int lazy_range_add(uint32_t base, uint32_t clusters, uint32_t dir_id, uint32_t kind,
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
void lazy_range_remove_owner(uint32_t dir_id, uint32_t kind)
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
void lazy_range_remove_data_cluster(uint32_t cluster)
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
int lazy_range_find(uint32_t cluster, struct proxy_lazy_range *found)
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
