// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_volume.c
 * @brief View geometry, reservation allocation, boot region, FAT and system chains.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- reservation allocation ---- */

/**
 * @brief Hand out contiguous clusters from one partition reservation.
 *
 * @details The allocation bitmap is not touched: the whole reservation was
 * marked used before the volume was registered. Only the partition bump moves.
 *
 * @param[in]  partition Partition index.
 * @param[in]  clusters  Number of clusters; zero yields an empty extent.
 * @param[out] first     Receives the first cluster, or zero.
 * @param[out] count     Receives the cluster count, or zero.
 * @return 0 on success, or -ENOSPC when the reservation is exhausted.
 */
int lazy_alloc_clusters(uint32_t partition, uint32_t clusters, uint32_t *first, uint32_t *count)
{
	struct proxy_lazy_partition *part;

	if (partition >= PROXY_LAZY_PARTITIONS) {
		return -EINVAL;
	}
	part = &lazy.partitions[partition];
	if (clusters == 0U) {
		*first = 0U;
		*count = 0U;
		return 0;
	}
	if ((uint64_t)part->bump + clusters > (uint64_t)part->reserve_first + part->reserve_clusters) {
		return -ENOSPC;
	}
	*first = part->bump;
	*count = clusters;
	part->bump += clusters;
	return 0;
}

/**
 * @brief Look up the next cluster of a host-written FAT chain.
 *
 * @param[in] cluster Cluster number.
 * @return The next cluster, or zero when the host did not write that entry.
 */
uint32_t lazy_fat_next(uint32_t cluster)
{
	for (uint32_t index = 0U; index < lazy.fat_count; index++) {
		if (lazy.fat_log[index].used && lazy.fat_log[index].cluster == cluster) {
			return lazy.fat_log[index].next;
		}
	}
	return 0U;
}

/**
 * @brief Remember one FAT entry the host wrote.
 *
 * @param[in] cluster Cluster number.
 * @param[in] next    Next cluster value.
 * @return 0 on success, -ENOSPC when the log is full, or -ENOMEM.
 */
int lazy_fat_set(uint32_t cluster, uint32_t next)
{
	for (uint32_t index = 0U; index < lazy.fat_count; index++) {
		if (lazy.fat_log[index].used && lazy.fat_log[index].cluster == cluster) {
			lazy.fat_log[index].next = next;
			return 0;
		}
	}
	if (next == 0U) {
		/* A cleared entry only matters for a cluster the log already knows. */
		return 0;
	}
	if (lazy.fat_count >= CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES) {
		return -ENOSPC;
	}
	lazy.fat_log[lazy.fat_count].cluster = cluster;
	lazy.fat_log[lazy.fat_count].next = next;
	lazy.fat_log[lazy.fat_count].used = true;
	lazy.fat_count++;
	return 0;
}

/**
 * @brief Parse one FAT sector the host wrote.
 *
 * @param[in] fat_sector Sector offset inside the FAT.
 * @param[in] data       Sector contents.
 * @return 0 on success, or -ENOSPC when the FAT log is full.
 */
int lazy_fat_write(uint32_t fat_sector, const uint8_t data[PROXY_LAZY_SECTOR_SIZE])
{
	uint32_t first = fat_sector * (PROXY_LAZY_SECTOR_SIZE / 4U);
	int result = 0;

	for (uint32_t index = 0U; index < PROXY_LAZY_SECTOR_SIZE / 4U; index++) {
		uint32_t value = (uint32_t)data[index * 4U] |
			((uint32_t)data[index * 4U + 1U] << 8) |
			((uint32_t)data[index * 4U + 2U] << 16) |
			((uint32_t)data[index * 4U + 3U] << 24);

		if (value == 0U && lazy_fat_next(first + index) == 0U) {
			continue;
		}
		result = lazy_fat_set(first + index, value);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

/* ---- view geometry ---- */

/**
 * @brief Compute the reservation of one selected partition.
 *
 * @details ext volumes reserve one view cluster per used 4 KiB of data plus a
 * small margin for directory extents; the source block already rounds each
 * file to at least one 4 KiB block, so the old per-inode cluster no longer
 * belongs in the sum. FAT volumes have no inode count, so the larger of the
 * view cluster count and the source cluster count covers a source cluster
 * smaller than the view cluster; a fixed file allowance covers directory data.
 *
 * @param[in,out] part Partition to size.
 * @return 0 on success, or -EFBIG when the reservation does not fit a cluster count.
 */
int lazy_compute_reservation(struct proxy_lazy_partition *part)
{
	uint64_t used_bytes;
	uint64_t used_clusters;
	uint64_t reservation;

	if (part->status != 0) {
		part->reserve_clusters = 8U;
		return 0;
	}
	used_bytes = part->volume.total_bytes > part->volume.free_bytes ?
		part->volume.total_bytes - part->volume.free_bytes : 0U;
	used_clusters = (used_bytes + PROXY_LAZY_CLUSTER_SIZE - 1U) / PROXY_LAZY_CLUSTER_SIZE;
	if (strncmp(part->volume.fstype, "ext", 3U) == 0) {
		reservation = used_clusters + used_clusters / 16U + 8U;
	} else {
		uint64_t counted = used_clusters;

		/*
		 * The larger of the view cluster count and the source cluster count
		 * covers both a small-cluster volume with many tiny files and a
		 * large-cluster volume whose view rounds every file up to a view
		 * cluster.
		 */
		if (part->volume.cluster_bytes != 0U) {
			uint64_t source_clusters = used_bytes / part->volume.cluster_bytes;

			if (source_clusters > counted) {
				counted = source_clusters;
			}
		}
		reservation = counted + CONFIG_STORAGE_PROXY_LAZY_FILE_ALLOWANCE + 8U;
	}
	if (reservation < 8U) {
		reservation = 8U;
	}
	if (reservation > 0x7ffffffdU) {
		return -EFBIG;
	}
	part->reserve_clusters = (uint32_t)reservation;
	return 0;
}

/**
 * @brief Size the view, build its system areas and mark every reservation used.
 *
 * @details The total cluster count follows from the up-case table, the root
 * directory, the partition reservations and the free tail. The allocation
 * bitmap itself occupies clusters too, so the count is solved iteratively. The
 * bitmap is allocated once and has every cluster up to the tail marked used;
 * nothing changes it later.
 *
 * @return 0 on success, or a negative errno.
 */
int lazy_compute_geometry(void)
{
	uint64_t base = (uint64_t)lazy.root_clusters + lazy.tail_clusters;
	uint64_t total;
	uint64_t bitmap_bytes;
	uint64_t bitmap_clusters;
	uint32_t next_cluster;
	int result;

	result = lazy_build_upcase();
	if (result != 0) {
		return result;
	}
	lazy.upcase_clusters = (lazy.upcase_bytes + PROXY_LAZY_CLUSTER_SIZE - 1U) /
		PROXY_LAZY_CLUSTER_SIZE;
	base += lazy.upcase_clusters;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		if (lazy.partitions[partition].selected) {
			base += lazy.partitions[partition].reserve_clusters;
		}
	}
	total = base;
	for (uint32_t attempt = 0U; attempt < 8U; attempt++) {
		uint64_t next;

		bitmap_bytes = (total + 7U) / 8U;
		bitmap_clusters = (bitmap_bytes + PROXY_LAZY_CLUSTER_SIZE - 1U) /
			PROXY_LAZY_CLUSTER_SIZE;
		next = base + bitmap_clusters;
		if (next == total) {
			break;
		}
		total = next;
	}
	if (total > 0x7ffffffdU) {
		return -EFBIG;
	}
	lazy.total_clusters = (uint32_t)total;
	lazy.bitmap_bytes = (uint32_t)((total + 7U) / 8U);
	lazy.bitmap_clusters = (uint32_t)(((uint64_t)lazy.bitmap_bytes +
		PROXY_LAZY_CLUSTER_SIZE - 1U) / PROXY_LAZY_CLUSTER_SIZE);
	lazy.bitmap = lazy_calloc(1U, lazy.bitmap_bytes);
	if (lazy.bitmap == NULL) {
		return -ENOMEM;
	}
	lazy.fat_sectors = (uint32_t)(((uint64_t)(lazy.total_clusters + 2U) * 4U +
		PROXY_LAZY_SECTOR_SIZE - 1U) / PROXY_LAZY_SECTOR_SIZE);
	lazy.data_start = (PROXY_LAZY_FAT_OFFSET + lazy.fat_sectors +
		PROXY_LAZY_SECTORS_PER_CLUSTER_U32 - 1U) & ~(PROXY_LAZY_SECTORS_PER_CLUSTER_U32 - 1U);
	lazy.total_sectors = lazy.data_start +
		(uint64_t)lazy.total_clusters * PROXY_LAZY_SECTORS_PER_CLUSTER_U32;
	if (lazy.total_sectors > UINT32_MAX) {
		return -EFBIG;
	}
	lazy.bitmap_cluster = PROXY_LAZY_ROOT_CLUSTER_MIN;
	lazy.upcase_cluster = lazy.bitmap_cluster + lazy.bitmap_clusters;
	lazy.root_cluster = lazy.upcase_cluster + lazy.upcase_clusters;
	next_cluster = lazy.root_cluster + lazy.root_clusters;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];

		if (!part->selected) {
			continue;
		}
		part->reserve_first = next_cluster;
		part->bump = next_cluster;
		next_cluster += part->reserve_clusters;
	}
	lazy.tail_first = next_cluster;
	/* The bitmap is final before the disk exists: mark every reservation used. */
	for (uint32_t cluster = PROXY_LAZY_ROOT_CLUSTER_MIN; cluster < lazy.tail_first; cluster++) {
		uint32_t bit = cluster - PROXY_LAZY_ROOT_CLUSTER_MIN;

		lazy.bitmap[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
	}
	lazy.dirs[PROXY_LAZY_ROOT_ID].first_cluster = lazy.root_cluster;
	lazy.dirs[PROXY_LAZY_ROOT_ID].clusters = lazy.root_clusters;
	lazy.dirs[PROXY_LAZY_ROOT_ID].extent_ready = true;
	result = lazy_range_add(lazy.root_cluster, lazy.root_clusters, PROXY_LAZY_ROOT_ID,
		PROXY_LAZY_RANGE_DIRECTORY, PROXY_LAZY_INVALID_SLOT, 0U);
	if (result == 0) {
		lazy.dirs[PROXY_LAZY_ROOT_ID].range_ready = true;
	}
	return result;
}

/* ---- boot region ---- */

/**
 * @brief Build one sector of the exFAT boot region.
 *
 * @details Records 1 through 8 are empty sectors with only the 0x55AA
 * signature. Record 0 holds the main boot sector; the used-percentage byte is
 * computed from the clusters the reservations hold.
 *
 * @param[in]  record Boot record number, 0 through 11.
 * @param[out] sector Sector buffer to fill.
 */
void lazy_boot_record(uint32_t record, uint8_t sector[PROXY_LAZY_SECTOR_SIZE])
{
	memset(sector, 0, PROXY_LAZY_SECTOR_SIZE);
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
	lazy_put_le64(sector + 64U, 0U);
	lazy_put_le64(sector + 72U, lazy.total_sectors);
	lazy_put_le32(sector + 80U, PROXY_LAZY_FAT_OFFSET);
	lazy_put_le32(sector + 84U, lazy.fat_sectors);
	lazy_put_le32(sector + 88U, lazy.data_start);
	lazy_put_le32(sector + 92U, lazy.total_clusters);
	lazy_put_le32(sector + 96U, lazy.root_cluster);
	lazy_put_le32(sector + 100U, 0x4c5a5956U);
	lazy_put_le16(sector + 104U, 0x0100U);
	lazy_put_le16(sector + 106U, 0U);
	sector[108] = 9U;
	sector[109] = (uint8_t)PROXY_LAZY_SECTORS_PER_CLUSTER_SHIFT;
	sector[110] = 1U;
	sector[111] = 0x80U;
	if (lazy.total_clusters == 0U) {
		sector[112] = 0U;
	} else {
		uint32_t used = lazy.tail_first - PROXY_LAZY_ROOT_CLUSTER_MIN;
		uint32_t percent = used * 100U / lazy.total_clusters;

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
 * @details The volume flags and the used-percentage byte are excluded, as the
 * exFAT specification requires.
 *
 * @return The boot region checksum.
 */
uint32_t lazy_boot_checksum(void)
{
	uint8_t sector[PROXY_LAZY_SECTOR_SIZE];
	uint32_t checksum = 0U;

	for (uint32_t record = 0U; record < 11U; record++) {
		lazy_boot_record(record, sector);
		for (uint32_t index = 0U; index < PROXY_LAZY_SECTOR_SIZE; index++) {
			if (record == 0U && (index == 106U || index == 107U || index == 112U)) {
				continue;
			}
			checksum = lazy_checksum32(checksum, sector[index]);
		}
	}
	return checksum;
}

/**
 * @brief FAT chain value for one cluster of a system area.
 *
 * @param[in] cluster Cluster number.
 * @param[in] first   First cluster of the system area.
 * @param[in] count   Number of clusters in the system area.
 * @return The chain value, or zero when the cluster is not in the area.
 */
uint32_t lazy_system_chain(uint32_t cluster, uint32_t first, uint32_t count)
{
	if (cluster < first || cluster >= first + count) {
		return 0U;
	}
	return cluster + 1U < first + count ? cluster + 1U : PROXY_LAZY_EOC;
}

/**
 * @brief Return the FAT entry for a cluster.
 *
 * @details The bitmap, the up-case table and the root directory are chained in
 * their own runs. Every other cluster returns zero, because objects are
 * contiguous NoFatChain streams.
 *
 * @param[in] cluster Cluster number.
 * @return The chain value stored in the FAT.
 */
uint32_t lazy_fat_entry(uint32_t cluster)
{
	uint32_t value;

	if (cluster == 0U) {
		return 0xfffffff8U;
	}
	if (cluster == 1U) {
		return PROXY_LAZY_EOC;
	}
	/* Extents the host chained on the FAT are served back from the write log. */
	value = lazy_fat_next(cluster);
	if (value != 0U) {
		return value;
	}
	value = lazy_system_chain(cluster, lazy.bitmap_cluster, lazy.bitmap_clusters);
	if (value != 0U) {
		return value;
	}
	value = lazy_system_chain(cluster, lazy.upcase_cluster, lazy.upcase_clusters);
	if (value != 0U) {
		return value;
	}
	if (cluster >= lazy.root_cluster && cluster < lazy.root_cluster + lazy.root_clusters) {
		return lazy_system_chain(cluster, lazy.root_cluster, lazy.root_clusters);
	}
	return 0U;
}
