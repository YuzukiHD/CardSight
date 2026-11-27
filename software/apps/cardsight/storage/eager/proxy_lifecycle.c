// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_lifecycle.c
 * @brief Start, stop and source-volume teardown of the eager view.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/**
 * @brief Unmount every source partition that is still mounted.
 */
void proxy_unmount_sources(void)
{
	for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
		if (proxy.partitions[partition].mounted) {
			(void)vfs_umount(proxy.partitions[partition].mount);
			proxy.partitions[partition].mounted = false;
		}
	}
}


/**
 * @brief Release everything the view holds.
 *
 * @details Any queued write is flushed, staging runs are released with their
 * files, the cached descriptor and the staging descriptor are closed, and the
 * source volumes are unmounted. The index, the string arena, the up-case table,
 * the bitmap and the pending table are freed and their pointers and counters
 * cleared, so a later start begins from a clean state. The split-group state is
 * reset too, because it refers to a directory of the old view.
 */
void proxy_release(void)
{
	(void)proxy_write_flush();
	proxy_stage_close();
	for (uint32_t index = 0U; index < PROXY_RUNS; index++) {
		proxy_run_release(&proxy.runs[index]);
	}
	if (proxy.cached_fd >= 0) {
		vfs_close(proxy.cached_fd);
		proxy.cached_fd = -1;
	proxy.stage_fd = -1;
	proxy.stage_path = NULL;
	proxy.write_length = 0U;
	proxy.split_slots = 0U;
	proxy.source_lost = false;
	}
	proxy.cached_node = PROXY_INVALID_NODE;
	proxy_unmount_sources();
	proxy_free(proxy.nodes);
	proxy_free(proxy.range_nodes);
	proxy_free(proxy.strings);
	proxy_free(proxy.upcase);
	proxy_free(proxy.bitmap);
	proxy.nodes = NULL;
	proxy.range_nodes = NULL;
	proxy.strings = NULL;
	proxy.upcase = NULL;
	proxy.bitmap = NULL;
	proxy.node_count = 0U;
	proxy.range_count = 0U;
	proxy.string_used = 0U;
	proxy.layout_ready = false;
	proxy_free(proxy.pending);
	proxy_free(proxy.pending_index);
	proxy.pending = NULL;
	proxy.pending_index = NULL;
	proxy.pending_count = 0U;
	proxy.pending_capacity = 0U;
	proxy.pending_index_size = 0U;
}


/**
 * @brief Start the view with an explicit partition selection.
 *
 * @details The selection mask must be non-empty and fit the partition limit.
 * Under @c proxy.lock the function:
 * 1. opens the source disk for its size and refuses a second start;
 * 2. probes every selected partition and records it as present, writable and
 *    selected, marking the whole view read-only when any source cannot be
 *    written;
 * 3. allocates the index, the cluster index and the string arena, creates the
 *    root node and mounts each selected partition as a folder;
 * 4. computes the geometry and adds the layout file, which describes the
 *    finished volume;
 * 5. indexes every mounted source directory so every file owns clusters before
 *    the host reads the allocation bitmap, then allocates the clusters;
 * 6. registers the block device with its capacity and read-only flag.
 * Any failure releases everything again, so a failed start leaves no partial
 * view behind.
 *
 * @param[in] config Source disk and selection mask.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_start_config(const struct storage_proxy_config *config)
{
	struct block_device *source_bdev;
	uint32_t partition_nodes[PROXY_PARTITIONS];
	const char *source_disk;
	int result = 0;

	if (config == NULL || config->source_disk == NULL || config->source_disk[0] == '\0' ||
		config->selected_partitions == 0U ||
		(config->selected_partitions & ~((1U << PROXY_PARTITIONS) - 1U)) != 0U) {
		return -EINVAL;
	}
	source_disk = config->source_disk;
	mutex_lock_timeout(&proxy.lock, WAIT_FOREVER);
	if (proxy.running) {
		mutex_unlock(&proxy.lock);
		return -EBUSY;
	}
	(void)__atomic_store_n(&proxy.armed, false, __ATOMIC_SEQ_CST);
	proxy.callbacks = 0U;
	source_bdev = bdev_open_by_name(source_disk);
	if (source_bdev == NULL) {
		mutex_unlock(&proxy.lock);
		return -ENODEV;
	}
	proxy.source_sectors = bdev_nr_sectors(source_bdev);
	snprintf(proxy.source_name, sizeof(proxy.source_name), "%s", source_disk);
	bdev_close(source_bdev);
	proxy.read_only = false;
	memset(proxy.partitions, 0, sizeof(proxy.partitions));
	for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
		struct proxy_partition *part = &proxy.partitions[partition];
		struct block_device *partition_bdev;

		part->selected = (config->selected_partitions & (1U << partition)) != 0U;
		part->status = -ENODEV;
		if (!part->selected) {
			continue;
		}
		snprintf(part->device, sizeof(part->device), "%sp%u", source_disk, partition + 1U);
		snprintf(part->mount, sizeof(part->mount), "/proxy%u", partition + 1U);
		partition_bdev = bdev_open_by_name(part->device);
		if (partition_bdev == NULL) {
			proxy.read_only = true;
			continue;
		}
		part->present = true;
		part->start_sector = partition_bdev->start_sect;
		part->sectors = partition_bdev->nr_sects;
		part->type = partition_bdev->part_type;
		part->writable = !bdev_read_only(partition_bdev);
		bdev_close(partition_bdev);
		part->status = vfs_probe(part->device, &part->volume);
		part->writable = part->status == 0 && part->writable && !part->volume.read_only;
		if (!part->writable) {
			proxy.read_only = true;
		}
	}
	proxy.truncated = false;
	proxy.layout_ready = false;
	proxy.nodes = proxy_calloc(CONFIG_STORAGE_PROXY_MAX_ENTRIES, sizeof(*proxy.nodes));
	proxy.range_nodes = proxy_alloc(CONFIG_STORAGE_PROXY_MAX_ENTRIES * sizeof(*proxy.range_nodes));
	proxy.strings = proxy_alloc(CONFIG_STORAGE_PROXY_STRING_BYTES);
	if (proxy.nodes == NULL || proxy.range_nodes == NULL || proxy.strings == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	proxy.cached_fd = -1;
	proxy.stage_fd = -1;
	proxy.stage_path = NULL;
	proxy.write_length = 0U;
	proxy.split_slots = 0U;
	proxy.source_lost = false;
	proxy.clean = true;
	proxy.cached_node = PROXY_INVALID_NODE;
	proxy.node_count = 1U;
	proxy.nodes[0].parent = 0U;
	proxy.nodes[0].first_child = PROXY_INVALID_NODE;
	proxy.nodes[0].last_child = PROXY_INVALID_NODE;
	proxy.nodes[0].next_sibling = PROXY_INVALID_NODE;
	proxy.nodes[0].directory = true;
	proxy.nodes[0].scanned = true;
	proxy.nodes[0].name_offset = proxy_store_string("");
	for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
		struct proxy_partition *part = &proxy.partitions[partition];

		if (!part->selected) {
			continue;
		}
		result = proxy_mount_partition(partition, &partition_nodes[partition]);
		if (result != 0) {
			goto fail;
		}
		if (part->mounted) {
			proxy.nodes[partition_nodes[partition]].source = PROXY_SOURCE_VFS;
			proxy.nodes[partition_nodes[partition]].scanned = false;
			proxy.nodes[partition_nodes[partition]].source_offset = proxy_store_string(part->mount);
			if (proxy.nodes[partition_nodes[partition]].source_offset == UINT32_MAX) {
				result = -ENOMEM;
				goto fail;
			}
		}
	}
	/* The geometry first: the layout file describes the finished volume, its size included. */
	result = proxy_compute_geometry();
	if (result != 0) {
		goto fail;
	}
	(void)proxy_layout_add_file();
	/*
	 * Every source file must own its clusters before the host reads the allocation bitmap. A host
	 * keeps that bitmap and hands out the clusters it shows as free, so a file indexed later would
	 * share them with a new one and be mistaken for it.
	 */
	for (uint32_t index = 0U; index < proxy.node_count; index++) {
		if (proxy.nodes[index].directory && proxy.nodes[index].source == PROXY_SOURCE_VFS &&
			!proxy.nodes[index].scanned) {
			(void)proxy_scan_directory(index);
		}
	}
	result = proxy_allocate_clusters();
	if (result != 0) {
		goto fail;
	}
	proxy.disk = alloc_disk();
	if (proxy.disk == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	strcpy(proxy.disk->disk_name, PROXY_DISK_NAME);
	proxy.disk->private_data = &proxy;
	proxy.disk->fops = &proxy_disk_operations;
	proxy.disk->capacity = proxy.total_sectors;
	proxy.disk->logical_block_size = PROXY_SECTOR_SIZE;
	proxy.disk->flags = proxy.read_only ? GENHD_FL_RDONLY : 0U;
	/* add_disk() reads the new disk's partition table through these callbacks. */
	(void)__atomic_store_n(&proxy.armed, true, __ATOMIC_SEQ_CST);
	result = add_disk(proxy.disk);
	if (result != 0) {
		(void)__atomic_store_n(&proxy.armed, false, __ATOMIC_SEQ_CST);
		put_disk(proxy.disk);
		proxy.disk = NULL;
		goto fail;
	}
	proxy.running = true;
	{
		char capacity[16];

		format_capacity(proxy.total_sectors * 512U, capacity, sizeof(capacity));
		pr_info("storage proxy: %u entries, %s exFAT view on %s\n", proxy.node_count, capacity,
			PROXY_DISK_NAME);
	}
	mutex_unlock(&proxy.lock);
	return 0;
fail:
	(void)__atomic_store_n(&proxy.armed, false, __ATOMIC_SEQ_CST);
	proxy_release();
	mutex_unlock(&proxy.lock);
	return result;
}


/**
 * @brief Start the view over every partition found on the disk.
 *
 * @details Every partition that exists, so a card with fewer than three is not
 * forced read only. The mask is built by opening each "diskN" name in turn and
 * handed to @c storage_proxy_start_config().
 *
 * @param[in] source_disk Block device name of the whole disk.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_start(const char *source_disk)
{
	struct storage_proxy_config config = { .source_disk = source_disk };

	for (uint32_t partition = 0U; source_disk != NULL && partition < PROXY_PARTITIONS; partition++) {
		char device[VFS_BLK_NAME];
		struct block_device *bdev;

		snprintf(device, sizeof(device), "%sp%u", source_disk, partition + 1U);
		bdev = bdev_open_by_name(device);
		if (bdev != NULL) {
			config.selected_partitions |= 1U << partition;
			bdev_close(bdev);
		}
	}
	return storage_proxy_start_config(&config);
}


/**
 * @brief Mark the source card as gone.
 *
 * @details The card went away: from now on the view only fails, so a stop
 * cannot wait on the dead volumes. Every callback checks this flag before it
 * touches a source path.
 */
void storage_proxy_source_lost(void)
{
	proxy.source_lost = true;
}


/**
 * @brief Stop the view and release everything it holds.
 *
 * @details Under @c proxy.lock the block device is removed. A removal that
 * cannot finish cleanly is forced, because the host may still hold the device
 * open while the card is being removed. The disk object is put, the running
 * flag cleared, and the buffers released.
 */
void storage_proxy_stop(void)
{
	/* Refuse new callbacks, then let the ones already inside the block layer return. */
	(void)__atomic_store_n(&proxy.armed, false, __ATOMIC_SEQ_CST);
#ifndef STORAGE_PROXY_HOST_TEST
	{
		uint64_t deadline = proxy_time_begin() + 2000000ULL;

		while (__atomic_load_n(&proxy.callbacks, __ATOMIC_SEQ_CST) != 0U &&
		       proxy_time_begin() < deadline) {
			msleep(10);
		}
	}
#endif
	mutex_lock_timeout(&proxy.lock, WAIT_FOREVER);
	if (proxy.running) {
		if (del_gendisk(proxy.disk) != 0) {
			del_gendisk_force(proxy.disk);
		}
		put_disk(proxy.disk);
		proxy.disk = NULL;
		proxy.running = false;
		proxy_release();
	}
	mutex_unlock(&proxy.lock);
}
