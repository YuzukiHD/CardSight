// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_volume.c
 * @brief The volume's own layout: boot sector, FAT, bitmap, up-case table and cluster map.
 */

#include "proxy_private.h"


/**
 * @brief Set or clear the allocation bit of one cluster.
 *
 * @details Cluster numbers outside the volume and a missing bitmap are
 * ignored, which lets the caller mark a range without bounds checks of its
 * own.
 *
 * @param[in] cluster Cluster number.
 * @param[in] used    True to mark the cluster used.
 */
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


/**
 * @brief Fold one byte into the exFAT rotating 32-bit checksum.
 *
 * @param[in] checksum Current checksum.
 * @param[in] value    Byte to add.
 * @return The updated checksum.
 */
static uint32_t proxy_checksum32(uint32_t checksum, uint8_t value)
{
	return ((checksum & 1U) != 0U ? 0x80000000U : 0U) +
		(checksum >> 1) + value;
}


/**
 * @brief Build the up-case table the view advertises.
 *
 * @details Each UTF-16 code unit is mapped through the source's upper-case
 * conversion. Runs of 128 or more units that map to themselves are compressed
 * into the exFAT identity-run encoding (0xFFFF followed by the run length),
 * which is why the builder is a small state machine. The checksum is computed
 * over the bytes exactly as they are stored. The table always fills all 65536
 * code units before the loop ends at the wrap to zero.
 *
 * @return 0 on success, or -ENOMEM.
 */
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


/**
 * @brief Give one node its clusters from the unallocated tail.
 *
 * @details A directory takes one cluster; a file takes as many clusters as its
 * size needs. The clusters are taken contiguously from @c next_cluster, marked
 * in the bitmap, added to the cluster index, and the tail is advanced. A node
 * with no size gets no cluster at all.
 *
 * @param[in] index Node index to allocate for.
 * @return 0 on success, or -ENOSPC when the view has no room left.
 */
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


/**
 * @brief Size the view and place its FAT, bitmap, up-case table and root cluster.
 *
 * @details The shape of the view: the cluster counts, where the FAT, the
 * bitmap, the up-case table and the root directory sit, and how many sectors
 * the volume spans. The nodes are not placed yet, so the layout file can
 * describe the finished volume before its clusters are handed out. The view is
 * sized from the source disk plus one directory cluster and one data cluster
 * per possible index entry, with slack; the up-case table is built first so
 * its length is known. The FAT is rounded to whole sectors and the data area
 * is aligned to a cluster boundary after it.
 *
 * @return 0 on success, or a negative errno.
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


/**
 * @brief Hand clusters to every node before a host reads the allocation bitmap.
 *
 * @details The bitmap, the up-case table and the root directory are placed
 * first at fixed cluster numbers and marked used. The root node is entered in
 * the cluster index, @c layout_ready is raised so later nodes allocate as they
 * are added, and every remaining node is allocated in index order. The cluster
 * index is rebuilt once the extents are final.
 *
 * @return 0 on success, or a negative errno.
 */
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


/**
 * @brief Find the node that owns a cluster.
 *
 * @details The cluster-sorted index is searched with binary search for the
 * node whose [first_cluster, first_cluster + cluster_count) range contains the
 * cluster.
 *
 * @param[in] cluster Cluster number.
 * @return The node index, or PROXY_INVALID_NODE.
 */
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


/**
 * @brief FAT chain value for one cluster of a system area.
 *
 * @details Returns the next cluster when the cluster is inside [first,
 * first + count), the end-of-chain marker for the last cluster of the range,
 * and zero when the cluster is outside it.
 *
 * @param[in] cluster Cluster number.
 * @param[in] first   First cluster of the system area.
 * @param[in] count   Number of clusters in the system area.
 * @return The chain value, or zero when the cluster is not in the area.
 */
static uint32_t proxy_system_chain(uint32_t cluster, uint32_t first, uint32_t count)
{
	if (cluster < first || cluster >= first + count) {
		return 0U;
	}
	return cluster + 1U < first + count ? cluster + 1U : PROXY_EOC;
}


/**
 * @brief Return the FAT entry for a cluster.
 *
 * @details Entries zero and one are the reserved media and end-of-chain
 * values. The bitmap, the up-case table and the root directory are chained in
 * their own runs; every other cluster returns zero, because objects are
 * contiguous streams and their FAT chain is not used.
 *
 * @param[in] cluster Cluster number.
 * @return The chain value stored in the FAT.
 */
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


/**
 * @brief Build one sector of the exFAT boot region.
 *
 * @details Records 1 through 8 are empty sectors carrying only the 0x55AA
 * signature. Record 0 holds the main boot sector: jump stub, "EXFAT   " file
 * system name, volume length, FAT and data offsets, cluster count, root
 * cluster, volume serial number, version, volume flags, the sector/cluster
 * geometry and the used-percentage field, which is computed from the clusters
 * already handed out. Every built sector ends with the signature.
 *
 * @param[in]  record Boot record number, 0 through 11.
 * @param[out] sector Sector buffer to fill.
 */
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


/**
 * @brief Compute the checksum of the first eleven boot records.
 *
 * @details The volume flags and the used-percentage byte of the main boot
 * sector are excluded, as exFAT requires, and the remaining bytes of records 0
 * to 10 are folded into the checksum. The result fills the checksum sector
 * that follows them.
 *
 * @return The boot region checksum.
 */
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


/**
 * @brief Drop cached data for clusters the host just marked free.
 *
 * @details For every bit that changed from used to free, the sixteen sectors
 * of the cluster are dropped from the pending table and a staging run that
 * starts at the cluster is released. Clusters the host freed hold no data
 * worth keeping.
 *
 * @param[in] byte_offset Offset of the bitmap byte the write starts at.
 * @param[in] incoming    Bytes the host wrote.
 * @param[in] length      Number of bytes to examine.
 */
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
