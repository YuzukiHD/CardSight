// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_pending.c
 * @brief RAM pending table for host data that arrives before its entry.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/**
 * @brief Hash a (cluster, sector) pair into the pending index.
 *
 * @details The pair is packed, multiplied by a large odd constant and shifted
 * right, which spreads neighbouring sectors across the table.
 *
 * @param[in] cluster Cluster of the sector.
 * @param[in] sector  Sector offset inside the cluster.
 * @return Hash value; the caller masks it with the table size.
 */
uint32_t proxy_pending_hash(uint32_t cluster, uint16_t sector)
{
	return ((cluster * 64U + sector) * 2654435761U) >> 7;
}


/**
 * @brief Rebuild the pending hash index from the array.
 *
 * @details The table is sized to at least twice the capacity so probe
 * sequences stay short, then every live entry is inserted with linear probing.
 * The old table is freed only after the new one is complete. Rebuild the index
 * from the array; after a flush compacts it or the array grows.
 *
 * @param[in] capacity Capacity of the pending array.
 * @return 0 on success, or -ENOMEM.
 */
int proxy_pending_reindex(uint32_t capacity)
{
	uint32_t size = 1U;
	uint32_t *index;

	while (size < capacity * 2U) {
		size <<= 1;
	}
	index = proxy_calloc(size, sizeof(*index));
	if (index == NULL) {
		return -ENOMEM;
	}
	for (uint32_t entry = 0U; entry < proxy.pending_count; entry++) {
		uint32_t position = proxy_pending_hash(proxy.pending[entry].cluster, proxy.pending[entry].sector) & (size - 1U);

		while (index[position] != 0U) {
			position = (position + 1U) & (size - 1U);
		}
		index[position] = entry + 1U;
	}
	proxy_free(proxy.pending_index);
	proxy.pending_index = index;
	proxy.pending_index_size = size;
	return 0;
}


/**
 * @brief Store one sector in the pending table.
 *
 * @details A rewrite of a sector already waiting replaces its contents in
 * place. When the array is full it doubles, up to PROXY_PENDING_MAX, copies
 * the live entries and rebuilds the hash index; at the limit the caller is
 * told there is no space and must fall back to staging on the card. A new
 * entry is appended and its index slot filled by linear probing.
 *
 * @param[in] cluster Cluster of the sector.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, -ENOSPC at the limit, or -ENOMEM.
 */
int proxy_pending_store_inner(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE])
{
	struct proxy_pending_sector *pending;
	uint32_t position;

	/* A rewrite of the same sector replaces it. */
	if (proxy.pending_index_size != 0U) {
		position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
		while (proxy.pending_index[position] != 0U) {
			pending = &proxy.pending[proxy.pending_index[position] - 1U];
			if (pending->cluster == cluster && pending->sector == sector) {
				memcpy(pending->data, data, PROXY_SECTOR_SIZE);
				pending->used = true;
				return 0;
			}
			position = (position + 1U) & (proxy.pending_index_size - 1U);
		}
	}
	if (proxy.pending_count == proxy.pending_capacity) {
		uint32_t capacity = proxy.pending_capacity == 0U ? 256U : proxy.pending_capacity * 2U;
		struct proxy_pending_sector *grown;

		if (capacity > PROXY_PENDING_MAX) {
			capacity = PROXY_PENDING_MAX;
		}
		if (capacity == proxy.pending_capacity) {
			return -ENOSPC;
		}
		grown = proxy_alloc((size_t)capacity * sizeof(*grown));
		if (grown == NULL) {
			return -ENOMEM;
		}
		if (proxy.pending_count != 0U) {
			memcpy(grown, proxy.pending, (size_t)proxy.pending_count * sizeof(*grown));
		}
		proxy_free(proxy.pending);
		proxy.pending = grown;
		proxy.pending_capacity = capacity;
		if (proxy_pending_reindex(capacity) != 0) {
			return -ENOMEM;
		}
	}
	pending = &proxy.pending[proxy.pending_count++];
	if (proxy.pending_count > proxy_stats.pending_peak) {
		proxy_stats.pending_peak = proxy.pending_count;
	}
	pending->used = true;
	pending->cluster = cluster;
	pending->sector = sector;
	memcpy(pending->data, data, PROXY_SECTOR_SIZE);
	position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
	while (proxy.pending_index[position] != 0U) {
		position = (position + 1U) & (proxy.pending_index_size - 1U);
	}
	proxy.pending_index[position] = proxy.pending_count;
	return 0;
}


/**
 * @brief Store one sector in the pending table, with pending-store timing.
 *
 * @param[in] cluster Cluster of the sector.
 * @param[in] sector  Sector offset inside the cluster.
 * @param[in] data    Sector contents.
 * @return 0 on success, -ENOSPC or -ENOMEM.
 */
int proxy_pending_store(uint32_t cluster, uint16_t sector, const uint8_t data[PROXY_SECTOR_SIZE])
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_pending_store_inner(cluster, sector, data);

	proxy_time_end(&proxy_stats.pending_store, begin);
	return result;
}


/**
 * @brief Write the pending sectors that belong to one node.
 *
 * @details Sectors that belong to the file are written in order, the rest stay
 * for another entry. The array is compacted in place while it is walked: a
 * sector is dropped once it is written, and everything else is kept. Entries
 * whose cluster was reused or freed are already marked unused and are dropped
 * without a write. Directory nodes only drop their sectors. The hash index is
 * rebuilt and the batch buffer flushed at the end.
 *
 * @param[in] node_index Node whose extent claims the sectors.
 * @return 0 on success, or a negative errno.
 */
int proxy_pending_flush_inner(uint32_t node_index)
{
	struct proxy_node *node = &proxy.nodes[node_index];
	uint32_t kept = 0U;
	int result = 0;

	for (uint32_t index = 0U; index < proxy.pending_count; index++) {
		struct proxy_pending_sector *pending = &proxy.pending[index];
		bool mine = pending->cluster >= node->first_cluster &&
			pending->cluster < node->first_cluster + node->cluster_count;

		if (!pending->used) {
			continue; /* dropped: its cluster was reused or freed */
		}
		if (mine && result == 0 && !node->directory) {
			result = proxy_write_node(node_index,
				((uint64_t)(pending->cluster - node->first_cluster) * PROXY_SECTORS_PER_CLUSTER +
					pending->sector) * PROXY_SECTOR_SIZE, pending->data);
			if (result == 0) {
				continue;
			}
		} else if (mine && node->directory) {
			continue;
		}
		if (kept != index) {
			proxy.pending[kept] = *pending;
		}
		kept++;
	}
	proxy.pending_count = kept;
	if (proxy.pending_capacity != 0U && proxy_pending_reindex(proxy.pending_capacity) != 0 && result == 0) {
		result = -ENOMEM;
	}
	if (result == 0) {
		result = proxy_write_flush();
	}
	return result;
}


/**
 * @brief Write the pending sectors of one node, with pending-flush timing.
 *
 * @param[in] node_index Node whose extent claims the sectors.
 * @return 0 on success, or a negative errno.
 */
int proxy_pending_flush(uint32_t node_index)
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_pending_flush_inner(node_index);
	proxy_time_end(&proxy_stats.pending_flush, begin);
	return result;
}


/**
 * @brief Find the source partition a path belongs to.
 *
 * @details The mount targets are compared as prefixes and the character after
 * the prefix must be a separator or the end of the path, so "/proxy1" does not
 * match "/proxy10".
 *
 * @param[in] path Source path.
 * @return Partition index, or PROXY_PARTITIONS when none matches.
 */
uint32_t proxy_path_partition(const char *path)
{
	for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
		size_t length = strlen(proxy.partitions[partition].mount);

		if (proxy.partitions[partition].mounted && length != 0U &&
			strncmp(path, proxy.partitions[partition].mount, length) == 0 &&
			(path[length] == '/' || path[length] == '\0')) {
			return partition;
		}
	}
	return PROXY_PARTITIONS;
}


/**
 * @brief Report whether any sector of a cluster is waiting in RAM.
 *
 * @details Only every eighth sector is probed, which is enough to decide
 * whether a cluster is part of the current in-order stream; a false negative
 * only makes the staging run start earlier.
 *
 * @param[in] cluster Cluster to look for.
 * @return True when a waiting sector of the cluster was found.
 */
bool proxy_pending_has_cluster(uint32_t cluster)
{
	for (uint16_t sector = 0U; sector < PROXY_SECTORS_PER_CLUSTER; sector += 8U) {
		uint32_t position;

		if (proxy.pending_index_size == 0U) {
			return false;
		}
		position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
		while (proxy.pending_index[position] != 0U) {
			const struct proxy_pending_sector *pending = &proxy.pending[proxy.pending_index[position] - 1U];

			if (pending->used && pending->cluster == cluster && pending->sector == sector) {
				return true;
			}
			position = (position + 1U) & (proxy.pending_index_size - 1U);
		}
	}
	return false;
}


/**
 * @brief Forget the data held for one sector.
 *
 * @details Forget RAM data held for a sector: the host rewrote it elsewhere,
 * or freed the cluster. The entry stays in the array but is marked unused, so
 * the next compaction drops it; the index is left alone.
 *
 * @param[in] cluster Cluster the sector belongs to.
 * @param[in] sector  Sector offset inside the cluster.
 */
void proxy_pending_drop(uint32_t cluster, uint16_t sector)
{
	uint32_t position;

	if (proxy.pending_index_size == 0U) {
		return;
	}
	position = proxy_pending_hash(cluster, sector) & (proxy.pending_index_size - 1U);
	while (proxy.pending_index[position] != 0U) {
		struct proxy_pending_sector *pending = &proxy.pending[proxy.pending_index[position] - 1U];

		if (pending->cluster == cluster && pending->sector == sector) {
			pending->used = false;
			return;
		}
		position = (position + 1U) & (proxy.pending_index_size - 1U);
	}
}
