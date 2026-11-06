// SPDX-License-Identifier: GPL-2.0-or-later
/* The synthesized volume's public interface: open, close and describe the view. */

#include "proxy_internal.h"

#include <size.h>

/* The view and its counters, the only instances; every other module reaches them through the header. */
struct storage_proxy proxy;
struct proxy_statistics proxy_stats;

/* The C library heap is small, so everything the view allocates comes from the kernel heap. */
#ifdef STORAGE_PROXY_HOST_TEST
void *proxy_alloc(size_t size)
{
	return malloc(size);
}

void proxy_free(void *pointer)
{
	free(pointer);
}
#else
void *proxy_alloc(size_t size)
{
	return pvPortMalloc(size);
}

void proxy_free(void *pointer)
{
	vPortFree(pointer);
}
#endif

void *proxy_calloc(size_t count, size_t size)
{
	void *pointer;

	if (count != 0U && size > SIZE_MAX / count) {
		return NULL;
	}
	pointer = proxy_alloc(count * size);
	if (pointer != NULL) {
		memset(pointer, 0, count * size);
	}
	return pointer;
}


static void proxy_unmount_sources(void)
{
	for (uint32_t partition = 0U; partition < PROXY_PARTITIONS; partition++) {
		if (proxy.partitions[partition].mounted) {
			(void)vfs_umount(proxy.partitions[partition].mount);
			proxy.partitions[partition].mounted = false;
		}
	}
}


static void proxy_release(void)
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
	result = add_disk(proxy.disk);
	if (result != 0) {
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
	proxy_release();
	mutex_unlock(&proxy.lock);
	return result;
}


/* Every partition that exists, so a card with fewer than three is not forced read only. */
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


/* The card went away: from now on the view only fails, so a stop cannot wait on the dead volumes. */
void storage_proxy_source_lost(void)
{
	proxy.source_lost = true;
}


void storage_proxy_stop(void)
{
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


bool storage_proxy_running(void)
{
	return proxy.running;
}


bool storage_proxy_read_only(void)
{
	return proxy.read_only;
}


int storage_proxy_scan(const char *source_disk, struct storage_proxy_partition_info *partitions,
	size_t capacity, size_t *count)
{
	struct block_device *source;
	size_t found = 0U;

	if (source_disk == NULL || partitions == NULL || count == NULL) {
		return -EINVAL;
	}
	source = bdev_open_by_name(source_disk);
	if (source == NULL || !bdev_is_whole(source)) {
		if (source != NULL) {
			bdev_close(source);
		}
		return -ENODEV;
	}
	bdev_close(source);
	for (uint32_t index = 0U; index < PROXY_PARTITIONS && found < capacity; index++) {
		struct storage_proxy_partition_info *info = &partitions[found];
		struct block_device *bdev;

		memset(info, 0, sizeof(*info));
		snprintf(info->device, sizeof(info->device), "%sp%u", source_disk, index + 1U);
		bdev = bdev_open_by_name(info->device);
		if (bdev == NULL) {
			continue;
		}
		info->present = true;
		info->partno = index + 1U;
		info->sectors = bdev_nr_sectors(bdev);
		info->writable = !bdev_read_only(bdev);
		bdev_close(bdev);
		info->status = vfs_probe(info->device, &info->volume);
		info->writable = info->status == 0 && info->writable && !info->volume.read_only;
		found++;
	}
	*count = found;
	return 0;
}


const char *storage_proxy_device_name(void)
{
	return PROXY_DISK_NAME;
}


size_t storage_proxy_entry_count(void)
{
	return proxy.node_count;
}


int storage_proxy_partition_status(uint32_t partition)
{
	return partition < PROXY_PARTITIONS ? proxy.partitions[partition].status : -EINVAL;
}


static int storage_proxy_init(void)
{
	memset(&proxy, 0, sizeof(proxy));
	proxy.cached_fd = -1;
	proxy.cached_node = PROXY_INVALID_NODE;
	mutex_init(&proxy.lock);
	return proxy.lock.handle == NULL ? -ENOMEM : 0;
}

INITCALL(3, 20, storage_proxy_init);
