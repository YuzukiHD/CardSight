// SPDX-License-Identifier: GPL-2.0-or-later
/* The volume's own layout: boot sector, FAT, allocation bitmap, up-case table and cluster map. */

#include "proxy_internal.h"


static void proxy_bitmap_set(uint32_t cluster, bool used)
{
	uint32_t bit;

	if (cluster < 2U || cluster >= proxy.total_clusters + 2U || proxy.bitmap == NULL) {
		return;
	}
	bit = cluster - 2U;
	if (used) {
		proxy.bitmap[bit / 8U] |= 1U << (bit % 8U);
	} else {
		proxy.bitmap[bit / 8U] &= ~(1U << (bit % 8U));
	}
}


static uint32_t proxy_checksum32(uint32_t checksum, uint8_t value)
{
	return ((checksum & 1U) != 0U ? 0x80000000U : 0U) +
		(checksum >> 1) + value;
}


static int proxy_build_upcase(void)
{
	uint16_t source = 0U;
	uint16_t character;
	uint32_t state = 0U;
	uint32_t run = 0U;
	uint32_t checksum = 0U;
	size_t offset = 0U;

	proxy.upcase = proxy_alloc(65536U * sizeof(uint16_t));
	if (proxy.upcase == NULL) {
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
		proxy.upcase[offset++] = (uint8_t)character;
		proxy.upcase[offset++] = (uint8_t)(character >> 8);
		checksum = proxy_checksum32(checksum, proxy.upcase[offset - 2U]);
		checksum = proxy_checksum32(checksum, proxy.upcase[offset - 1U]);
	} while (source != 0U);
	proxy.upcase_bytes = (uint32_t)offset;
	proxy.upcase_checksum = checksum;
	return 0;
}


int proxy_allocate_node_clusters(uint32_t index)
{
	struct proxy_node *node = &proxy.nodes[index];
	uint64_t clusters = node->directory ? PROXY_DIRECTORY_CLUSTERS :
		(node->size + PROXY_CLUSTER_SIZE - 1U) / PROXY_CLUSTER_SIZE;

	if (clusters == 0U) {
		node->first_cluster = 0U;
		node->cluster_count = 0U;
		return 0;
	}
	if (clusters > UINT32_MAX || proxy.next_cluster + clusters > proxy.total_clusters + 2U) {
		return -ENOSPC;
	}
	node->first_cluster = proxy.next_cluster;
	node->cluster_count = (uint32_t)clusters;
	proxy.range_nodes[proxy.range_count++] = index;
	for (uint32_t cluster = node->first_cluster; cluster < node->first_cluster + node->cluster_count; cluster++) {
		proxy_bitmap_set(cluster, true);
	}
	proxy.next_cluster += (uint32_t)clusters;
	return 0;
}


/*
 * The shape of the view: the cluster counts, where the FAT, the bitmap, the up-case table and the
 * root directory sit, and how many sectors the volume spans. The nodes are not placed yet, so the
 * layout file can describe the finished volume before its clusters are handed out.
 */
int proxy_compute_geometry(void)
{
	uint64_t clusters = (proxy.source_sectors + PROXY_SECTORS_PER_CLUSTER - 1U) /
		PROXY_SECTORS_PER_CLUSTER;
	int result = proxy_build_upcase();

	if (result != 0) {
		return result;
	}
	clusters += (uint64_t)CONFIG_STORAGE_PROXY_MAX_ENTRIES *
		(PROXY_DIRECTORY_CLUSTERS + 1U) + 64U;
	if (clusters > 0x7ffffffdU) {
		return -EFBIG;
	}
	proxy.total_clusters = (uint32_t)clusters;
	proxy.bitmap_bytes = (proxy.total_clusters + 7U) / 8U;
	proxy.bitmap_clusters = (proxy.bitmap_bytes + PROXY_CLUSTER_SIZE - 1U) / PROXY_CLUSTER_SIZE;
	proxy.bitmap = proxy_calloc(1U, proxy.bitmap_bytes);
	if (proxy.bitmap == NULL) {
		return -ENOMEM;
	}
	proxy.upcase_clusters = (proxy.upcase_bytes + PROXY_CLUSTER_SIZE - 1U) / PROXY_CLUSTER_SIZE;
	proxy.fat_sectors = (uint32_t)(((uint64_t)(proxy.total_clusters + 2U) * 4U +
					     PROXY_SECTOR_SIZE - 1U) / PROXY_SECTOR_SIZE);
	proxy.data_start = (PROXY_FAT_OFFSET + proxy.fat_sectors +
		PROXY_SECTORS_PER_CLUSTER - 1U) & ~(PROXY_SECTORS_PER_CLUSTER - 1U);
	proxy.total_sectors = proxy.data_start +
		(uint64_t)proxy.total_clusters * PROXY_SECTORS_PER_CLUSTER;
	if (proxy.total_sectors > UINT32_MAX) {
		return -EFBIG;
	}
	return 0;
}


/* Every node owns its clusters before a host reads the allocation bitmap. */
int proxy_allocate_clusters(void)
{
	int result = 0;

	proxy.bitmap_cluster = PROXY_ROOT_CLUSTER_MIN;
	proxy.upcase_cluster = proxy.bitmap_cluster + proxy.bitmap_clusters;
	proxy.root_cluster = proxy.upcase_cluster + proxy.upcase_clusters;
	proxy.next_cluster = proxy.root_cluster + 1U;
	proxy.range_count = 1U;
	proxy.range_nodes[0] = 0U;
	proxy.nodes[0].first_cluster = proxy.root_cluster;
	proxy.nodes[0].cluster_count = 1U;
	for (uint32_t cluster = proxy.bitmap_cluster; cluster <= proxy.root_cluster; cluster++) {
		proxy_bitmap_set(cluster, true);
	}
	proxy.layout_ready = true;
	for (uint32_t index = 1U; index < proxy.node_count; index++) {
		result = proxy_allocate_node_clusters(index);
		if (result != 0) {
			return result;
		}
	}
	proxy_rebuild_ranges();
	return 0;
}


uint32_t proxy_cluster_node(uint32_t cluster)
{
	uint32_t low = 0U;
	uint32_t high = proxy.range_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		uint32_t node_index = proxy.range_nodes[middle];
		const struct proxy_node *node = &proxy.nodes[node_index];

		if (cluster < node->first_cluster) {
			high = middle;
		} else if (cluster >= node->first_cluster + node->cluster_count) {
			low = middle + 1U;
		} else {
			return node_index;
		}
	}
	return PROXY_INVALID_NODE;
}


static uint32_t proxy_system_chain(uint32_t cluster, uint32_t first, uint32_t count)
{
	if (cluster < first || cluster >= first + count) {
		return 0U;
	}
	return cluster + 1U < first + count ? cluster + 1U : PROXY_EOC;
}


uint32_t proxy_fat_entry(uint32_t cluster)
{
	uint32_t value;

	if (cluster == 0U) {
		return 0xfffffff8U;
	}
	if (cluster == 1U) {
		return PROXY_EOC;
	}
	value = proxy_system_chain(cluster, proxy.bitmap_cluster, proxy.bitmap_clusters);
	if (value != 0U) {
		return value;
	}
	value = proxy_system_chain(cluster, proxy.upcase_cluster, proxy.upcase_clusters);
	if (value != 0U) {
		return value;
	}
	if (cluster == proxy.root_cluster) {
		return PROXY_EOC;
	}
	return 0U;
}


void proxy_boot_record(uint32_t record, uint8_t sector[PROXY_SECTOR_SIZE])
{
	memset(sector, 0, PROXY_SECTOR_SIZE);
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
	put_le64(sector + 64U, 0U);
	put_le64(sector + 72U, proxy.total_sectors);
	put_le32(sector + 80U, PROXY_FAT_OFFSET);
	put_le32(sector + 84U, proxy.fat_sectors);
	put_le32(sector + 88U, proxy.data_start);
	put_le32(sector + 92U, proxy.total_clusters);
	put_le32(sector + 96U, proxy.root_cluster);
	put_le32(sector + 100U, 0x43534947U);
	put_le16(sector + 104U, 0x0100U);
	put_le16(sector + 106U, proxy.volume_flags);
	sector[108] = 9U;
	sector[109] = 6U;
	sector[110] = 1U;
	sector[111] = 0x80U;
	if (proxy.total_clusters == 0U) {
		sector[112] = 0U;
	} else {
		uint32_t percent = (proxy.next_cluster - 2U) * 100U / proxy.total_clusters;

		sector[112] = (uint8_t)(percent < 100U ? percent : 100U);
	}
	sector[120] = 0xebU;
	sector[121] = 0xfeU;
	sector[510] = 0x55U;
	sector[511] = 0xaaU;
}


uint32_t proxy_boot_checksum(void)
{
	uint8_t sector[PROXY_SECTOR_SIZE];
	uint32_t checksum = 0U;

	for (uint32_t record = 0U; record < 11U; record++) {
		proxy_boot_record(record, sector);
		for (uint32_t index = 0U; index < PROXY_SECTOR_SIZE; index++) {
			if (record == 0U && (index == 106U || index == 107U || index == 112U)) {
				continue;
			}
			checksum = proxy_checksum32(checksum, sector[index]);
		}
	}
	return checksum;
}


/* Clusters the host just freed hold no data worth keeping: drop what was waiting for them. */
void proxy_bitmap_freed(uint64_t byte_offset, const uint8_t *incoming, size_t length)
{
	for (size_t index = 0U; index < length; index++) {
		uint8_t freed = proxy.bitmap[byte_offset + index] & (uint8_t)~incoming[index];

		for (uint32_t bit = 0U; freed != 0U && bit < 8U; bit++, freed >>= 1) {
			uint32_t cluster;

			if ((freed & 1U) == 0U) {
				continue;
			}
			cluster = (uint32_t)((byte_offset + index) * 8U + bit) + 2U;
			for (uint16_t sector = 0U; sector < PROXY_SECTORS_PER_CLUSTER; sector++) {
				proxy_pending_drop(cluster, sector);
			}
			for (uint32_t run_index = 0U; run_index < PROXY_RUNS; run_index++) {
				struct proxy_run *run = &proxy.runs[run_index];

				if (run->used && cluster == run->first_cluster) {
					proxy_run_release(run);
				}
			}
		}
	}
}
