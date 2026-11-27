// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_lifecycle.c
 * @brief Start, stop, probe, release and root construction of the view.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/** @brief The one lazy view instance. */
struct proxy_lazy lazy;


/* ---- start and stop ---- */

/**
 * @brief Drop cached state and unmount every source partition.
 *
 * @details The caller has already removed the block device, so nothing can
 * reach the view while its buffers are freed.
 */
void lazy_release(void)
{
	lazy_write_cache_drop();
	if (lazy.cached_fd >= 0) {
		(void)vfs_close(lazy.cached_fd);
		lazy.cached_fd = -1;
	}
	/* Staging files live on the sources, so they must go before the unmounts. */
	lazy_run_release_all();
	lazy_free(lazy.runs);
	lazy.runs = NULL;
	lazy.claiming = false;
	if (lazy.dirs != NULL) {
		for (uint32_t index = 0U; index < lazy.dir_count; index++) {
			lazy_free(lazy.dirs[index].entries);
			lazy_free(lazy.dirs[index].names);
			lazy_free(lazy.dirs[index].subdirs);
		}
		lazy_free(lazy.dirs);
	}
	lazy.dirs = NULL;
	lazy.dir_count = 0U;
	lazy.dir_capacity = 0U;
	lazy_free(lazy.name_arena);
	lazy.name_arena = NULL;
	lazy.name_used = 0U;
	lazy_free(lazy.ranges);
	lazy.ranges = NULL;
	lazy.range_count = 0U;
	lazy.range_capacity = 0U;
	lazy_free(lazy.bitmap);
	lazy.bitmap = NULL;
	lazy_free(lazy.upcase);
	lazy.upcase = NULL;
	lazy_free(lazy.overlay);
	lazy.overlay = NULL;
	lazy_free(lazy.overlay_names);
	lazy.overlay_names = NULL;
	lazy_free(lazy.pending);
	lazy.pending = NULL;
	lazy_free(lazy.fat_log);
	lazy.fat_log = NULL;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		if (lazy.partitions[partition].mounted) {
			(void)vfs_umount(lazy.partitions[partition].mount);
			lazy.partitions[partition].mounted = false;
		}
	}
	lazy.bitmap_bytes = 0U;
	lazy.bitmap_clusters = 0U;
	lazy.upcase_bytes = 0U;
	lazy.upcase_clusters = 0U;
	lazy.upcase_checksum = 0U;
	lazy.total_clusters = 0U;
	lazy.total_sectors = 0U;
	lazy.data_start = 0U;
	lazy.fat_sectors = 0U;
	lazy.bitmap_cluster = 0U;
	lazy.root_cluster = 0U;
	lazy.root_clusters = 0U;
	lazy.tail_first = 0U;
	lazy.tail_clusters = 0U;
	lazy.root_entry_count = 0U;
	lazy.overlay_count = 0U;
	lazy.overlay_name_used = 0U;
	lazy.pending_count = 0U;
	lazy.staged_sectors = 0U;
	lazy.fat_count = 0U;
	lazy.split_slots = 0U;
	lazy.split_dir = PROXY_LAZY_INVALID_DIR;
	lazy.split_offset = 0U;
	lazy.write_fd = -1;
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.write_position = 0U;
	lazy.cached_fd = -1;
	lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	lazy.cached_slot = PROXY_LAZY_INVALID_DIR;
	lazy.cached_position = 0U;
	lazy.cache_entries = 0U;
	lazy.cache_bytes = 0U;
	lazy.lru_head = PROXY_LAZY_INVALID_DIR;
	lazy.lru_tail = PROXY_LAZY_INVALID_DIR;
	lazy.truncated = false;
	lazy.last_error = 0;
	lazy.evictions = 0U;
	lazy.sectors_read = 0U;
	lazy.sectors_written = 0U;
	lazy.file_bytes = 0U;
	lazy.file_bytes_written = 0U;
	lazy.clean = true;
	lazy.flush_failed = false;
}

/**
 * @brief Build the synthesized informational files of the root directory.
 *
 * @details One cluster per report is taken from the first selected partition's
 * reservation, so the files have a stable extent and never disturb the tail the
 * host allocates from. Both reports are read-only and regenerate their text on
 * every read; a partition that cannot spare a cluster simply omits them.
 *
 * @param[in,out] slot Next free absolute root slot.
 */
void lazy_create_root_files(uint32_t *slot)
{
	static const struct {
		const char *name;
		uint32_t kind;
	} files[PROXY_LAZY_ROOT_FILES] = {
		{ "_LAYOUT.TXT", PROXY_LAZY_ROOT_LAYOUT },
		{ "_DIR_LIMIT.TXT", PROXY_LAZY_ROOT_LIMITS },
	};
	uint32_t partition = PROXY_LAZY_PARTITIONS;

	for (uint32_t index = 0U; index < PROXY_LAZY_PARTITIONS; index++) {
		if (lazy.partitions[index].selected) {
			partition = index;
			break;
		}
	}
	if (partition >= PROXY_LAZY_PARTITIONS) {
		return;
	}
	for (uint32_t index = 0U; index < PROXY_LAZY_ROOT_FILES; index++) {
		struct proxy_lazy_root_entry *entry;
		uint32_t first;
		uint32_t clusters;

		if (lazy.root_entry_count >=
		    sizeof(lazy.root_entries) / sizeof(lazy.root_entries[0])) {
			return;
		}
		if (lazy_alloc_clusters(partition, 1U, &first, &clusters) != 0) {
			return;
		}
		entry = &lazy.root_entries[lazy.root_entry_count++];
		memset(entry, 0, sizeof(*entry));
		entry->kind = files[index].kind;
		entry->slot = *slot;
		entry->entries = lazy_name_entries(files[index].name);
		entry->first_cluster = first;
		entry->clusters = clusters;
		*slot += entry->entries;
	}
}

/**
 * @brief Build the partition folders and their root entries.
 *
 * @return 0 on success, or -ENOSPC when the header table or name arena is full.
 */
int lazy_create_partition_folders(void)
{
	uint32_t slot = PROXY_LAZY_ROOT_SYSTEM_SLOTS;

	lazy.root_entry_count = 0U;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];
		char name[16];
		uint32_t identifier;

		if (!part->selected) {
			continue;
		}
		snprintf(name, sizeof(name), "partition%u", partition + 1U);
		identifier = lazy_dir_alloc(name);
		if (identifier == UINT32_MAX) {
			return -ENOSPC;
		}
		lazy.dirs[identifier].parent = PROXY_LAZY_ROOT_ID;
		lazy.dirs[identifier].partition = partition;
		lazy.dirs[identifier].is_partition_root = true;
		part->dir_id = identifier;
		lazy.root_entries[lazy.root_entry_count].kind = PROXY_LAZY_ROOT_FOLDER;
		lazy.root_entries[lazy.root_entry_count].dir_id = identifier;
		lazy.root_entries[lazy.root_entry_count].slot = slot;
		lazy.root_entries[lazy.root_entry_count].entries = lazy_name_entries(name);
		slot += lazy_name_entries(name);
		lazy.root_entry_count++;
	}
	lazy_create_root_files(&slot);
	lazy.root_host_slot = slot;
#ifndef STORAGE_PROXY_HOST_TEST
	/*
	 * Windows creates "System Volume Information" in the root of every volume
	 * it mounts read-write. Give that entry a real backing directory on the
	 * first selected partition so the mount does not fight a read-only root;
	 * the entry is rendered as part of the synthetic root. The host test keeps
	 * its source tree pristine and does not exercise the Windows root helper.
	 */
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];
		char path[VFS_PATH_MAX];
		uint32_t identifier;
		const char *helper = "System Volume Information";

		if (!part->selected || !part->mounted || part->mount[0] == '\0') {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", part->mount, helper);
		(void)vfs_mkdir(path); /* -EEXIST is fine: the directory may already be there */
		identifier = lazy_dir_alloc(helper);
		if (identifier == UINT32_MAX) {
			break;
		}
		lazy.dirs[identifier].parent = part->dir_id;
		lazy.dirs[identifier].partition = partition;
		lazy.root_entries[lazy.root_entry_count].kind = PROXY_LAZY_ROOT_FOLDER;
		lazy.root_entries[lazy.root_entry_count].dir_id = identifier;
		lazy.root_entries[lazy.root_entry_count].slot = slot;
		lazy.root_entries[lazy.root_entry_count].entries = lazy_name_entries(helper);
		slot += lazy_name_entries(helper);
		lazy.root_entry_count++;
		break;
	}
#endif
	lazy.dirs[PROXY_LAZY_ROOT_ID].entry_count = slot + 1U;
	return 0;
}

/**
 * @brief Map the probed file system name to a registered VFS type.
 *
 * @details The probe reports the exact generation ("fat32", "ext4") while the
 * VFS registers one type for a family. Mapping here avoids probing every type
 * a second time and the log lines a wrong guess would print.
 *
 * @param[in] info Volume information from the probe.
 * @return A registered type name, or NULL when the type is not known.
 */
const char *lazy_mount_type(const struct vfs_volume_info *info)
{
	if (strncmp(info->fstype, "ext", 3U) == 0) {
		return "ext";
	}
	if (strcmp(info->fstype, "fat32") == 0) {
		return "fat32";
	}
	if (strcmp(info->fstype, "exfat") == 0) {
		return "exfat";
	}
	if (strncmp(info->fstype, "fat", 3U) == 0) {
		return "fat";
	}
	return NULL;
}

/**
 * @brief Open the source disk for its size and describe the selected partitions.
 *
 * @param[in] source_disk Block device name of the whole source disk.
 * @return 0 on success, or -ENODEV when the disk is not registered.
 */
int lazy_probe_partitions(const char *source_disk)
{
	struct block_device *source = bdev_open_by_name(source_disk);

	if (source == NULL) {
		return -ENODEV;
	}
	snprintf(lazy.source_name, sizeof(lazy.source_name), "%s", source_disk);
	bdev_close(source);
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];
		struct block_device *bdev;

		if (!part->selected) {
			continue;
		}
		snprintf(part->device, sizeof(part->device), "%sp%u", source_disk, partition + 1U);
		snprintf(part->mount, sizeof(part->mount), "/lazy%u", partition + 1U);
		part->status = -ENODEV;
		bdev = bdev_open_by_name(part->device);
		if (bdev == NULL) {
			continue;
		}
		part->present = true;
		part->start_sector = bdev->start_sect;
		part->sectors = bdev->nr_sects;
		part->type = bdev->part_type;
		bdev_close(bdev);
		part->status = vfs_probe(part->device, &part->volume);
		if (part->status == 0) {
			/* The lazy view writes host changes back to the source. */
			int mounted = vfs_mount_ex(part->device, part->mount,
				lazy_mount_type(&part->volume), 0U);

			part->mounted = mounted == 0;
			if (!part->mounted) {
				part->status = mounted;
			}
		}
	}
	return 0;
}

/**
 * @brief Start the lazy view with an explicit partition selection.
 *
 * @details Startup runs in six stages: describe the selected partitions, size
 * their reservations, allocate the persistent tables, build the system areas,
 * create the partition folders and register the block device. Any failure
 * unmounts the sources and frees everything again.
 *
 * @param[in] source_disk          Whole-disk block device name.
 * @param[in] selected_partitions Bit mask of partitions; bit zero is partition one.
 * @return 0 on success, or a negative errno.
 */
int storage_proxy_lazy_start(const char *source_disk, uint32_t selected_partitions)
{
	uint32_t selected_count = 0U;
	uint32_t root_slots;
	int result = 0;

	if (source_disk == NULL || source_disk[0] == '\0' || selected_partitions == 0U ||
	    (selected_partitions & ~((1U << PROXY_LAZY_PARTITIONS) - 1U)) != 0U) {
		return -EINVAL;
	}
	result = mutex_lock_timeout(&lazy.lock, WAIT_FOREVER);
	if (result != 0) {
		return result;
	}
	if (lazy.running || lazy.disk != NULL) {
		mutex_unlock(&lazy.lock);
		return -EBUSY;
	}
	(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
	lazy.callbacks = 0U;
	memset(lazy.partitions, 0, sizeof(lazy.partitions));
	lazy.running = false;
	lazy.truncated = false;
	(void)__atomic_store_n(&lazy.cancel, false, __ATOMIC_SEQ_CST);
	if (lazy.cache_entries_max == 0U || lazy.cache_bytes_max == 0U ||
	    lazy.dir_max_entries == 0U) {
		lazy_limits_default();
	}
	lazy.last_error = 0;
	lazy.evictions = 0U;
	lazy.sectors_read = 0U;
	lazy.sectors_written = 0U;
	lazy.file_bytes = 0U;
	lazy.file_bytes_written = 0U;
	lazy.clean = true;
	lazy.flush_failed = false;
	lazy.write_fd = -1;
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.write_position = 0U;
	lazy.split_slots = 0U;
	lazy.split_dir = PROXY_LAZY_INVALID_DIR;
	lazy.split_offset = 0U;
	lazy.cached_fd = -1;
	lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	lazy.cached_slot = PROXY_LAZY_INVALID_DIR;
	lazy.cached_position = 0U;
	lazy.cache_entries = 0U;
	lazy.cache_bytes = 0U;
	lazy.lru_head = PROXY_LAZY_INVALID_DIR;
	lazy.lru_tail = PROXY_LAZY_INVALID_DIR;
	lazy.tail_clusters = 0U;
	lazy.upcase = NULL;
	lazy.upcase_bytes = 0U;
	lazy.upcase_clusters = 0U;
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		lazy.partitions[partition].selected =
			(selected_partitions & (1U << partition)) != 0U;
		if (lazy.partitions[partition].selected) {
			selected_count++;
		}
	}
	result = lazy_probe_partitions(source_disk);
	if (result != 0) {
		goto fail;
	}
	for (uint32_t partition = 0U; partition < PROXY_LAZY_PARTITIONS; partition++) {
		struct proxy_lazy_partition *part = &lazy.partitions[partition];

		if (!part->selected) {
			continue;
		}
		result = lazy_compute_reservation(part);
		if (result != 0) {
			goto fail;
		}
		/* The tail is backed by real free space on the sources. */
		{
			uint64_t free_clusters = part->volume.free_bytes / PROXY_LAZY_CLUSTER_SIZE;

			if (free_clusters > CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS) {
				free_clusters = CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS;
			}
			lazy.tail_clusters += (uint32_t)free_clusters;
		}
	}
	if (lazy.tail_clusters > CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS) {
		lazy.tail_clusters = CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS;
	}
	lazy.dir_capacity = CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS;
	lazy.dirs = lazy_calloc(lazy.dir_capacity, sizeof(*lazy.dirs));
	lazy.name_arena = lazy_alloc(CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES);
	lazy.overlay = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX,
		sizeof(*lazy.overlay));
	lazy.overlay_names = lazy_alloc(CONFIG_STORAGE_PROXY_LAZY_OVERLAY_NAME_BYTES);
	lazy.pending = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS,
		sizeof(*lazy.pending));
	lazy.runs = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_RUNS, sizeof(*lazy.runs));
	lazy.fat_log = lazy_calloc(CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES,
		sizeof(*lazy.fat_log));
	if (lazy.dirs == NULL || lazy.name_arena == NULL || lazy.overlay == NULL ||
	    lazy.overlay_names == NULL || lazy.pending == NULL || lazy.runs == NULL ||
	    lazy.fat_log == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	for (uint32_t index = 0U; index < CONFIG_STORAGE_PROXY_LAZY_RUNS; index++) {
		lazy.runs[index].fd = -1;
	}
	lazy.dir_count = 0U;
	lazy.name_used = 0U;
	lazy.overlay_count = 0U;
	lazy.overlay_name_used = 0U;
	lazy.pending_count = 0U;
	lazy.staged_sectors = 0U;
	lazy.fat_count = 0U;
	lazy.range_count = 0U;
	lazy.range_capacity = 0U;
	lazy.ranges = NULL;
	lazy.root_entry_count = 0U;
	{
		uint32_t root_identifier = lazy_dir_alloc("");

		if (root_identifier != PROXY_LAZY_ROOT_ID) {
			result = -ENOMEM;
			goto fail;
		}
		lazy.dirs[root_identifier].is_root = true;
		lazy.dirs[root_identifier].cache_ready = true;
		lazy.dirs[root_identifier].extent_ready = true;
	}
	/* Six extra slots hold the two synthesized root report files. */
	root_slots = PROXY_LAZY_ROOT_SYSTEM_SLOTS + selected_count * 3U + 6U + 1U;
	lazy.root_clusters = (root_slots + PROXY_LAZY_SLOTS_PER_CLUSTER - 1U) /
		PROXY_LAZY_SLOTS_PER_CLUSTER;
	if (lazy.root_clusters == 0U) {
		lazy.root_clusters = 1U;
	}
	/* Size the up-case table first: its cluster count feeds the geometry. */
	lazy.upcase_bytes = 0U;
	lazy.upcase_clusters = 0U;
	result = lazy_compute_geometry();
	if (result != 0) {
		goto fail;
	}
	result = lazy_create_partition_folders();
	if (result != 0) {
		goto fail;
	}
	lazy.disk = alloc_disk();
	if (lazy.disk == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	strcpy(lazy.disk->disk_name, PROXY_LAZY_DISK_NAME);
	lazy.disk->private_data = &lazy;
	lazy.disk->fops = &lazy_disk_operations;
	lazy.disk->capacity = lazy.total_sectors;
	lazy.disk->logical_block_size = PROXY_LAZY_SECTOR_SIZE;
	lazy.disk->flags = 0U;
	(void)__atomic_store_n(&lazy.armed, true, __ATOMIC_SEQ_CST);
	result = add_disk(lazy.disk);
	if (result != 0) {
		(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
		put_disk(lazy.disk);
		lazy.disk = NULL;
		goto fail;
	}
	lazy.running = true;
	lazy.usb_active = false;
	pr_info("lazy proxy: %s, %u clusters, %u reserved, %u tail, %u partitions\n",
		PROXY_LAZY_DISK_NAME, lazy.total_clusters, lazy.tail_first - 2U,
		lazy.tail_clusters, selected_count);
	mutex_unlock(&lazy.lock);
	return 0;
fail:
	(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
	lazy_release();
	mutex_unlock(&lazy.lock);
	return result;
}

/**
 * @brief Stop the lazy view and release everything it holds.
 *
 * @details New callbacks are refused first, then the ones already inside the
 * block layer are given a bounded time to return. Removing the disk and
 * freeing the caches run under the view lock.
 */
void storage_proxy_lazy_stop(void)
{
	int result;

	/*
	 * A long directory count or load runs under the view lock. Requesting
	 * cancellation before waiting for callbacks lets the load abort at its next
	 * progress check instead of making stop time out.
	 */
	(void)__atomic_store_n(&lazy.cancel, true, __ATOMIC_SEQ_CST);
#ifndef STORAGE_PROXY_HOST_TEST
	if (lazy.usb_active) {
		/* Quiesce the host first, exactly like the eager view's stop path. */
		(void)usb_mass_storage_stop();
		lazy.usb_active = false;
	}
#endif
	(void)__atomic_store_n(&lazy.armed, false, __ATOMIC_SEQ_CST);
#ifndef STORAGE_PROXY_HOST_TEST
	for (uint32_t attempt = 0U; attempt < 200U &&
	     __atomic_load_n(&lazy.callbacks, __ATOMIC_SEQ_CST) != 0U; attempt++) {
		msleep(10);
	}
#endif
	result = mutex_lock_timeout(&lazy.lock, PROXY_LAZY_LOCK_TIMEOUT_MS);
	if (result != 0) {
		pr_warn("lazy proxy: stop could not take the view lock: %d\n", result);
		return;
	}
	if (lazy.running) {
		lazy.running = false;
		/* One last flush carries out what the host wrote; the clean flag keeps it idempotent. */
		if (lazy_flush_locked() != 0) {
			pr_warn("lazy proxy: stop flush failed\n");
		}
		if (lazy.disk != NULL) {
			if (del_gendisk(lazy.disk) != 0) {
				del_gendisk_force(lazy.disk);
			}
			put_disk(lazy.disk);
			lazy.disk = NULL;
		}
		lazy_release();
	}
	mutex_unlock(&lazy.lock);
}

/**
 * @brief Initialise the lazy view's instance and its lock.
 *
 * @return 0 on success, or -ENOMEM when the lock cannot be created.
 */
int lazy_proxy_init(void)
{
	memset(&lazy, 0, sizeof(lazy));
	lazy.cached_fd = -1;
	lazy.cached_dir = PROXY_LAZY_INVALID_DIR;
	lazy.cached_slot = PROXY_LAZY_INVALID_DIR;
	lazy.write_fd = -1;
	lazy.write_dir = PROXY_LAZY_INVALID_DIR;
	lazy.write_slot = PROXY_LAZY_INVALID_SLOT;
	lazy.split_dir = PROXY_LAZY_INVALID_DIR;
	lazy.lru_head = PROXY_LAZY_INVALID_DIR;
	lazy.lru_tail = PROXY_LAZY_INVALID_DIR;
	lazy.clean = true;
	lazy_limits_default();
	mutex_init(&lazy.lock);
	return lazy.lock.handle == NULL ? -ENOMEM : 0;
}
/** @brief Register the lazy view initialiser with the init call table. */
INITCALL(3, 21, lazy_proxy_init);
