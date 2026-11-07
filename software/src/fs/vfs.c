/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file vfs.c
 * @brief Mount table, file descriptors, and path resolution for the virtual file system.
 *
 * One lock serialises the calls, except vfs_format() and vfs_probe(): they run for a long time
 * and claim the block device instead. vfs_os_lock()/vfs_os_unlock() come from the platform
 * (src/fs/vfs_os.c); the host tests provide their own.
 */

#define LOG_MODULE "vfs"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/blkdev.h>
#include <log.h>
#include <vfs.h>


/** @brief Fallback mount slot count when the configuration does not provide one. */
#ifndef CONFIG_VFS_MAX_MOUNTS
#define CONFIG_VFS_MAX_MOUNTS 8
#endif
/** @brief Fallback open file slot count when the configuration does not provide one. */
#ifndef CONFIG_VFS_MAX_FILES
#define CONFIG_VFS_MAX_FILES 16
#endif
/** @brief Fallback number of registered file system types. */
#ifndef CONFIG_VFS_MAX_FS_TYPES
#define CONFIG_VFS_MAX_FS_TYPES 8
#endif

/** @brief Longest mount point accepted, including the NUL terminator. */
#define TARGET_MAX 32

/**
 * @brief One entry of the mount table.
 *
 * The slots are allocated for the life of the system; @c used tells whether a slot currently
 * describes a mounted file system.
 */
struct vfs_mount {
	bool used;			/**< Slot in use: the entry describes a live mount. */
	char target[TARGET_MAX];	/**< Absolute mount point, for example "/sd". */
	struct block_device *bdev;	/**< Backing device, or NULL for a nodev file system. */
	const struct vfs_fs *fs;	/**< File system type that owns the mount. */
	void *priv;			/**< Per-mount context returned by the type's mount hook. */
	uint32_t open_count;		/**< Open files and directories below this mount. */
};

/**
 * @brief One entry of the open file table.
 */
struct vfs_file {
	struct vfs_mount *mount;	/**< Mount the file lives on. */
	void *handle;		/**< Handle owned by the file system type. */
};

/**
 * @brief An open directory.
 *
 * A directory with no mount is the synthetic root directory that lists the mount points.
 */
struct vfs_dir {
	struct vfs_mount *mount;	/**< NULL: the list of mount points. */
	void *handle;		/**< Handle owned by the file system type. */
	uint32_t index;		/**< Mount slot reached in the synthetic listing. */
};

/** @brief Registered file system types, in registration order; protected by the VFS lock. */
static const struct vfs_fs *fs_types[CONFIG_VFS_MAX_FS_TYPES];
/** @brief Mount slots; @c used marks the live ones. */
static struct vfs_mount mounts[CONFIG_VFS_MAX_MOUNTS];
/** @brief Open file slots, indexed by descriptor minus VFS_FD_BASE. */
static struct vfs_file files[CONFIG_VFS_MAX_FILES];
/** @brief Current working directory, absolute and normalised. */
static char cwd[VFS_PATH_MAX] = "/";

/**
 * @brief Live mounts sorted by target length, longest first: the deepest mount wins.
 */
static struct vfs_mount *mounts_by_target[CONFIG_VFS_MAX_MOUNTS];
/** @brief Number of live mounts in @c mounts_by_target. */
static uint32_t mount_count;

/* Weak defaults: the host tests and the target provide the real lock. */
/**
 * @brief Take the VFS lock.
 *
 * Weak default that does nothing; the platform (src/fs/vfs_os.c) or the host test provides the
 * real implementation.
 */
__attribute__((weak)) void vfs_os_lock(void) {}
/**
 * @brief Release the VFS lock.
 *
 * Weak default that does nothing; pairs with vfs_os_lock().
 */
__attribute__((weak)) void vfs_os_unlock(void) {}

/**
 * @brief Rebuild the longest-target-first order of the mount lookup table.
 *
 * Collects every live slot into @c mounts_by_target and insertion-sorts the array by target
 * length, descending, so mount_find_path() meets the deepest mount first. The VFS lock must be
 * held.
 */
static void mount_table_update(void)
{
	mount_count = 0U;
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_MOUNTS; index++) {
		if (mounts[index].used) {
			mounts_by_target[mount_count++] = &mounts[index];
		}
	}
	/* insertion sort, longest first */
	for (uint32_t index = 1U; index < mount_count; index++) {
		struct vfs_mount *entry = mounts_by_target[index];
		uint32_t position = index;

		while (position > 0U &&
		       strlen(mounts_by_target[position - 1U]->target) < strlen(entry->target)) {
			mounts_by_target[position] = mounts_by_target[position - 1U];
			position--;
		}
		mounts_by_target[position] = entry;
	}
}

/**
 * @brief Add a file system type to the VFS.
 *
 * @param[in] fs File system descriptor; @c name and @c mount must be set.
 * @return 0 on success, -EINVAL when @p fs or a mandatory field is NULL, -ENOMEM when the type
 *         table is full.
 *
 * Registration is idempotent: an already registered pointer returns 0. The lock is held while
 * the table changes, because lookups may run concurrently.
 */
int vfs_register(const struct vfs_fs *fs)
{
	int result = -ENOMEM;

	if (fs == NULL || fs->name == NULL || fs->mount == NULL) {
		return -EINVAL;
	}
	vfs_os_lock();
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_FS_TYPES; index++) {
		if (fs_types[index] == fs) {
			result = 0;
			break;
		}
		if (fs_types[index] == NULL) {
			fs_types[index] = fs;
			result = 0;
			break;
		}
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Remove a file system type from the VFS.
 *
 * @param[in] fs Descriptor previously passed to vfs_register().
 * @return 0 on success, -ENOENT when @p fs is not registered.
 *
 * The table is compacted over the hole, because lookups stop at the first NULL slot.
 */
int vfs_unregister(const struct vfs_fs *fs)
{
	int result = -ENOENT;

	vfs_os_lock();
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_FS_TYPES; index++) {
		if (fs_types[index] == fs) {
			for (uint32_t move = index; move + 1U < CONFIG_VFS_MAX_FS_TYPES; move++) {
				fs_types[move] = fs_types[move + 1U];
			}
			fs_types[CONFIG_VFS_MAX_FS_TYPES - 1U] = NULL;
			result = 0;
			break;
		}
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Find a registered file system type by name.
 *
 * @param[in] name Name as typed on the mount command line.
 * @return The descriptor, or NULL when no type carries that name.
 */
static const struct vfs_fs *fs_lookup(const char *name)
{
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_FS_TYPES && fs_types[index] != NULL; index++) {
		if (strcmp(fs_types[index]->name, name) == 0) {
			return fs_types[index];
		}
	}
	return NULL;
}

/* "/a/b" -> "/a/b"; "a/b" -> "/<cwd>/a/b"; "." and ".." resolved. */
/**
 * @brief Turn a path into an absolute, normalised path relative to the current directory.
 *
 * @param[in]  path     Input path, absolute or relative.
 * @param[out] absolute Buffer of VFS_PATH_MAX bytes receiving the result.
 * @return 0 on success, -EINVAL when @p path is NULL, -ENAMETOOLONG when the result does not
 *         fit.
 *
 * A relative path is joined to @c cwd. The result is then rebuilt component by component: empty
 * and "." components are dropped, ".." pops the previous component without ever leaving "/",
 * and components are separated by single slashes. The buffer is NUL-terminated on success.
 */
static int normalize(const char *path, char absolute[VFS_PATH_MAX])
{
	char combined[VFS_PATH_MAX];
	size_t length;

	if (path == NULL) {
		return -EINVAL;
	}
	if (path[0] == '/') {
		snprintf(combined, sizeof(combined), "%s", path);
	} else {
		snprintf(combined, sizeof(combined), "%s/%s", strcmp(cwd, "/") == 0 ? "" : cwd, path);
	}
	absolute[0] = '/';
	length = 1U;
	{
		const char *cursor = combined;

		while (*cursor != '\0') {
			const char *start;
			size_t count;

			while (*cursor == '/') {
				cursor++;
			}
			if (*cursor == '\0') {
				break;
			}
			start = cursor;
			while (*cursor != '\0' && *cursor != '/') {
				cursor++;
			}
			count = (size_t)(cursor - start);
			if (count == 1U && start[0] == '.') {
				continue;
			}
			if (count == 2U && start[0] == '.' && start[1] == '.') {
				while (length > 1U && absolute[length - 1U] != '/') {
					length--;
				}
				if (length > 1U) {
					length--;
				}
				absolute[length] = '\0';
				continue;
			}
			if (length > 1U) {
				if (length + 1U >= VFS_PATH_MAX) {
					return -ENAMETOOLONG;
				}
				absolute[length++] = '/';
			}
			if (length + count >= VFS_PATH_MAX) {
				return -ENAMETOOLONG;
			}
			memcpy(absolute + length, start, count);
			length += count;
			absolute[length] = '\0';
		}
	}
	return 0;
}

/**
 * @brief Find the mount that covers an absolute path.
 *
 * @param[in]  absolute Normalised absolute path.
 * @param[out] relative Receives the path inside the mount; "/" for the mount point itself.
 * @return The covering mount, or NULL when no mount point prefixes the path.
 *
 * The lookup table is ordered longest target first, so the first prefix match is the deepest
 * mount. A mount on "/" keeps the whole path; the others strip their own target. A prefix
 * matches only at a component boundary.
 */
static struct vfs_mount *mount_find_path(const char *absolute, const char **relative)
{
	for (uint32_t index = 0U; index < mount_count; index++) {
		struct vfs_mount *mount = mounts_by_target[index];
		size_t length = strlen(mount->target);

		if (strncmp(absolute, mount->target, length) == 0 &&
		    (absolute[length] == '\0' || absolute[length] == '/' || length == 1U)) {
			/* a mount on "/" keeps the whole path; the others give what follows them */
			const char *rest = length == 1U ? absolute : absolute + length;

			if (*rest == '\0') {
				rest = "/";
			}
			*relative = rest;
			return mount;
		}
	}
	return NULL;
}

/**
 * @brief Find a live mount by its exact mount point.
 *
 * @param[in] target Absolute target name.
 * @return The mount, or NULL when nothing is mounted there.
 */
static struct vfs_mount *mount_find_target(const char *target)
{
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_MOUNTS; index++) {
		if (mounts[index].used && strcmp(mounts[index].target, target) == 0) {
			return &mounts[index];
		}
	}
	return NULL;
}

/**
 * @brief Normalise a path and split it into its mount and the path inside it.
 *
 * @param[in]  path     Path to resolve.
 * @param[out] mount    Receives the covering mount.
 * @param[out] relative Buffer of VFS_PATH_MAX bytes receiving the in-mount path.
 * @return 0 on success, the error from normalize(), or -ENOENT when no mount covers the path.
 */
static int resolve(const char *path, struct vfs_mount **mount, char relative[VFS_PATH_MAX])
{
	char absolute[VFS_PATH_MAX];
	int result = normalize(path, absolute);

	if (result != 0) {
		return result;
	}
	*mount = mount_find_path(absolute, &path);
	if (*mount == NULL) {
		return -ENOENT;
	}
	snprintf(relative, VFS_PATH_MAX, "%s", path);
	return 0;
}

/* ---- mounting ---- */

/**
 * @brief Mount a block device or a nodev file system at a target.
 *
 * @param[in] blk_name Block device name, NULL for file systems flagged VFS_FS_NODEV.
 * @param[in] target   Mount point; one level only.
 * @param[in] fstype   File system name, or NULL to try every type.
 * @param[in] flags    VFS_MOUNT_* flags passed to the type's mount hook.
 * @return 0 on success, -EINVAL, -ENAMETOOLONG, -ENODEV, -EBUSY, -ENOMEM, or the error from
 *         the file system's mount hook.
 *
 * The VFS lock is held throughout. The target is normalised and rejected if deeper than one
 * level or too long. A named type is looked up; unless it is a nodev type, the block device is
 * opened (512-byte sectors only), a duplicate target is refused with -EBUSY and the device is
 * claimed so a second mount cannot take it. A free slot is reserved, the type's mount hook runs
 * (or each device-backed type is probed in registration order when none was named), and a
 * successful mount is recorded and re-sorted into the lookup table. Any failure releases the
 * claim and closes the device.
 */
int vfs_mount_ex(const char *blk_name, const char *target, const char *fstype, uint32_t flags)
{
	char absolute[VFS_PATH_MAX];
	struct vfs_mount *mount = NULL;
	struct block_device *bdev = NULL;
	const struct vfs_fs *fs = NULL;
	void *private_data = NULL;
	bool claimed = false;
	int result = normalize(target, absolute);

	vfs_os_lock();
	if (result == 0 && absolute[0] == '/' && strchr(absolute + 1, '/') != NULL) {
		result = -EINVAL; /* one level only */
	}
	if (result == 0 && strlen(absolute) >= TARGET_MAX) {
		result = -ENAMETOOLONG;
	}
	/* the type decides whether a device is needed at all */
	if (result == 0 && fstype != NULL) {
		fs = fs_lookup(fstype);
		if (fs == NULL) {
			result = -ENODEV;
		}
	}
	if (result == 0 && !(fs != NULL && (fs->flags & VFS_FS_NODEV) != 0U)) {
		bdev = blk_name != NULL ? bdev_open_by_name(blk_name) : NULL;
		if (bdev != NULL && bdev_sector_size(bdev) != 512U) {
			bdev_close(bdev);
			bdev = NULL;
		}
		if (bdev == NULL) {
			result = -ENODEV;
		}
	}
	if (result == 0 && mount_find_target(absolute) != NULL) {
		result = -EBUSY;
	}
	/* claim the device before the file system starts writing its superblock */
	if (result == 0 && bdev != NULL) {
		result = bdev_claim_mount(bdev);
		claimed = result == 0;
	}
	/* reserve a slot before touching the medium */
	for (uint32_t index = 0U; result == 0 && mount == NULL && index < CONFIG_VFS_MAX_MOUNTS; index++) {
		if (!mounts[index].used) {
			mount = &mounts[index];
		}
	}
	if (result == 0 && mount == NULL) {
		result = -ENOMEM;
	}
	if (result == 0 && fs != NULL) {
		result = fs->mount(bdev, flags, &private_data);
	} else if (result == 0) {
		result = -EINVAL;
		for (uint32_t index = 0U; index < CONFIG_VFS_MAX_FS_TYPES && fs_types[index] != NULL; index++) {
			if ((fs_types[index]->flags & VFS_FS_NODEV) == 0U &&
			    fs_types[index]->mount(bdev, flags, &private_data) == 0) {
				fs = fs_types[index];
				result = 0;
				break;
			}
		}
	}
	if (result == 0) {
		memset(mount, 0, sizeof(*mount));
		mount->used = true;
		snprintf(mount->target, sizeof(mount->target), "%s", absolute);
		mount->bdev = bdev;
		mount->fs = fs;
		mount->priv = private_data;
		mount_table_update();
	} else if (bdev != NULL) {
		if (claimed) {
			bdev_release_mount(bdev);
		}
		bdev_close(bdev);
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Mount a device at a target with default flags.
 *
 * @param[in] blk_name Block device name, or NULL for a nodev file system.
 * @param[in] target   Mount point.
 * @param[in] fstype   File system name, or NULL to probe.
 * @return 0 on success, or the negative errno from vfs_mount_ex().
 */
int vfs_mount(const char *blk_name, const char *target, const char *fstype)
{
	return vfs_mount_ex(blk_name, target, fstype, 0U);
}

/**
 * @brief Unmount the file system at a target.
 *
 * @param[in] target Mount point.
 * @return 0 on success, -EINVAL for an invalid or unknown target, -EBUSY while files or
 *         directories are open below it, or the type's umount error.
 *
 * Under the lock: validates the target, refuses while @c open_count is non-zero, runs the
 * type's umount hook (if any), releases the device claim and closes it, then frees the slot and
 * re-sorts the lookup table.
 */
int vfs_umount(const char *target)
{
	char absolute[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result = normalize(target, absolute);

	vfs_os_lock();
	if (result != 0) {
		/* keep the error */
	} else if (absolute[0] == '/' && strchr(absolute + 1, '/') != NULL) {
		result = -EINVAL;
	} else if ((mount = mount_find_target(absolute)) == NULL) {
		result = -EINVAL;
	} else if (mount->open_count != 0U) {
		result = -EBUSY;
	} else {
		result = mount->fs->umount != NULL ? mount->fs->umount(mount->priv) : 0;
		if (mount->bdev != NULL) {
			bdev_release_mount(mount->bdev);
			bdev_close(mount->bdev);
		}
		mount->used = false;
		mount->priv = NULL;
		mount->bdev = NULL;
		mount_table_update();
	}
	vfs_os_unlock();
	return result;
}

/* ---- formatting ---- */

/** @brief Progress callback installed by vfs_set_format_progress(), or NULL. */
static void (*format_progress)(void *context, uint64_t done, uint64_t total);
/** @brief Opaque pointer passed back to @c format_progress. */
static void *format_progress_context;
/** @brief True while a format of @c format_device is in progress. */
static bool format_active;
/** @brief Highest sector count reported so far in the running format. */
static uint64_t format_highest;
/** @brief Sectors written so far in the running format. */
static uint64_t format_written;
/** @brief Sectors the formatter expects to write; zero when it did not estimate. */
static uint64_t format_expected;
/** @brief Device being formatted, or NULL when no format runs. */
static struct block_device *format_device;

/**
 * @brief Install the callback that receives format progress.
 *
 * @param[in] callback Progress function, or NULL to disable reporting.
 * @param[in] context  Opaque pointer passed back to @p callback.
 */
void vfs_set_format_progress(void (*callback)(void *context, uint64_t done, uint64_t total), void *context)
{
	format_progress = callback;
	format_progress_context = context;
}

/**
 * @brief Declare how many sectors the running format will write.
 *
 * @param[in] bdev    Device being formatted.
 * @param[in] sectors Expected sector count; ignored when zero.
 *
 * Ignored unless a format of @p bdev is in progress. The formatter knows its write volume
 * better than the sector position does, because a FAT format only touches the start of the
 * card.
 */
void vfs_format_expect(struct block_device *bdev, uint64_t sectors)
{
	if (!format_active || bdev != format_device || sectors == 0U) {
		return;
	}
	format_expected = sectors;
}

/**
 * @brief Account sectors written by a formatter and report the new progress.
 *
 * @param[in] bdev    Device being formatted.
 * @param[in] sectors Number of sectors just written.
 *
 * A FAT format writes only the reserved area, the FATs and the root directory, all near the
 * start of the card: measuring progress by the sector position would sit at 0% and then jump to
 * 100%. The formatter instead estimates the sectors it will write with vfs_format_expect(); the
 * device size stands in when it did not. The written count accumulates with a high-water mark,
 * so a rewritten sector cannot pull the bar back, and the value is clamped to the total. 100%
 * is withheld until vfs_format() finishes, so only a completed format fills the bar.
 */
void vfs_format_report_write(struct block_device *bdev, uint64_t sectors)
{
	uint64_t total;
	uint64_t done;

	if (!format_active || format_progress == NULL || bdev != format_device) {
		return;
	}
	total = format_expected != 0U ? format_expected : bdev_nr_sectors(bdev);
	format_written += sectors;
	if (format_written > format_highest) {
		format_highest = format_written;
	}
	done = format_highest < total ? format_highest : total;
	if (done == total && total > 1U) {
		done = total - 1U; /* 100% belongs to the finished format */
	}
	format_progress(format_progress_context, done, total);
}

/**
 * @brief Format a block device with one file system.
 *
 * @param[in] blk_name Device name.
 * @param[in] fstype   File system name, or NULL for the first registered type.
 * @param[in] label    Volume label, may be NULL.
 * @param[in] arg      Type-specific argument: allocation unit, or the ext2/ext3/ext4 choice.
 * @return 0 on success, -ENODEV, -ENOTSUP when the type cannot format, or the formatter's
 *         error.
 *
 * The VFS lock is not taken, because a format is minutes of card I/O. Instead the block device
 * is opened and claimed, which keeps mounts out. The progress state is armed around the type's
 * format hook and disarmed afterwards; on success the callback is finally told the full size.
 * The claim and the device reference are dropped on every path.
 */
int vfs_format(const char *blk_name, const char *fstype, const char *label, uint32_t arg)
{
	const struct vfs_fs *fs;
	struct block_device *bdev;
	bool claimed = false;
	int result;

	/* A format is minutes of card I/O: the device is claimed, the lock is not held. */
	bdev = blk_name != NULL ? bdev_open_by_name(blk_name) : NULL;
	if (bdev != NULL && bdev_sector_size(bdev) != 512U) {
		bdev_close(bdev);
		bdev = NULL;
	}
	fs = fstype != NULL ? fs_lookup(fstype) : fs_types[0];
	if (bdev == NULL || fs == NULL) {
		result = -ENODEV;
	} else if (fs->format == NULL) {
		result = -ENOTSUP;
	} else if ((result = bdev_claim_mount(bdev)) != 0) {
		/* -EBUSY when something is mounted from it, -ENODEV when the disk is gone */
	} else {
		claimed = true;
		format_device = bdev;
		format_highest = 0U;
		format_written = 0U;
		format_expected = 0U;
		format_active = true;
		result = fs->format(bdev, label, arg);
		format_active = false;
		format_device = NULL;
		if (result == 0 && format_progress != NULL) {
			uint64_t total = format_expected != 0U ? format_expected : bdev_nr_sectors(bdev);

			format_progress(format_progress_context, total, total);
		}
	}
	if (claimed) {
		bdev_release_mount(bdev);
	}
	if (bdev != NULL) {
		bdev_close(bdev);
	}
	return result;
}

/**
 * @brief Probe a device and report the volume found on it.
 *
 * @param[in]  blk_name Device name.
 * @param[out] info     Zeroed, then filled with type, label, sizes and the read-only flag.
 * @return 0 on success, -EINVAL, -ENODEV, -EBUSY when the device is in use, or the error from
 *         the file system.
 *
 * The device is opened and claimed, then every registered device-backed type is tried with a
 * read-only mount until one succeeds. That mount supplies statfs() and volume_info(), and is
 * unmounted again. The claim replaces the VFS lock here: the probe mounts the volume itself.
 */
int vfs_probe(const char *blk_name, struct vfs_volume_info *info)
{
	struct block_device *bdev;
	int result = -EINVAL;

	if (blk_name == NULL || info == NULL) {
		return -EINVAL;
	}
	memset(info, 0, sizeof(*info));
	/* The probe mounts the volume itself, so it does not hold the lock; the claim excludes others. */
	bdev = bdev_open_by_name(blk_name);
	if (bdev == NULL || bdev_sector_size(bdev) != 512U) {
		result = -ENODEV;
	} else if ((result = bdev_claim_mount(bdev)) != 0) {
		/* -EBUSY when one of them is already mounted */
	} else {
		/* try each device-backed type until one recognises the volume */
		for (uint32_t index = 0U; index < CONFIG_VFS_MAX_FS_TYPES && fs_types[index] != NULL; index++) {
			const struct vfs_fs *fs = fs_types[index];
			void *private_data = NULL;

			if ((fs->flags & VFS_FS_NODEV) != 0U ||
			    fs->mount(bdev, VFS_MOUNT_READ_ONLY, &private_data) != 0) {
				continue;
			}
			snprintf(info->fstype, sizeof(info->fstype), "%s", fs->name);
			snprintf(info->label, sizeof(info->label), "%s", bdev->label);
			info->read_only = bdev_read_only(bdev) || (fs->flags & VFS_FS_READ_ONLY) != 0U;
			result = fs->statfs != NULL ? fs->statfs(private_data, &info->total_bytes, &info->free_bytes) : 0;
			if (result == 0 && fs->volume_info != NULL) {
				result = fs->volume_info(private_data, info);
			}
			if (fs->umount != NULL && result == 0) {
				result = fs->umount(private_data);
			} else if (fs->umount != NULL) {
				(void)fs->umount(private_data);
			}
			break;
		}
		bdev_release_mount(bdev);
	}
	if (bdev != NULL) {
		bdev_close(bdev);
	}
	return result;
}

/* ---- files ---- */

/**
 * @brief Translate a descriptor into its file slot.
 *
 * @param[in] descriptor Value returned by vfs_open().
 * @return The slot, or NULL when the number is out of range or the slot is not open.
 */
static struct vfs_file *file_get(int descriptor)
{
	if (descriptor < VFS_FD_BASE || descriptor >= VFS_FD_BASE + CONFIG_VFS_MAX_FILES) {
		return NULL;
	}
	if (files[descriptor - VFS_FD_BASE].mount == NULL) {
		return NULL;
	}
	return &files[descriptor - VFS_FD_BASE];
}

/**
 * @brief Open a file and return a VFS descriptor.
 *
 * @param[in] path  Path in the VFS namespace.
 * @param[in] flags VFS_O_* access and creation flags.
 * @return A descriptor at or above VFS_FD_BASE, or a negative errno.
 *
 * Under the lock: resolves the path, calls the type's open hook, then takes the first free file
 * slot. With every slot busy the handle is closed again and -EMFILE returned. The mount's
 * open_count is bumped and VFS_O_APPEND seeks to the end once here, so later writes append.
 */
int vfs_open(const char *path, uint32_t flags)
{
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	struct vfs_file *file = NULL;
	void *handle = NULL;
	int result;

	vfs_os_lock();
	result = resolve(path, &mount, relative);
	if (result == 0) {
		result = mount->fs->open(mount->priv, relative, flags, &handle);
	}
	if (result == 0) {
		/* the first free file slot */
		for (uint32_t index = 0U; index < CONFIG_VFS_MAX_FILES; index++) {
			if (files[index].mount == NULL) {
				file = &files[index];
				break;
			}
		}
		if (file == NULL) {
			if (mount->fs->close != NULL) {
				(void)mount->fs->close(handle);
			}
			result = -EMFILE;
		}
	}
	if (result == 0) {
		file->mount = mount;
		file->handle = handle;
		mount->open_count++;
		if ((flags & VFS_O_APPEND) != 0U && mount->fs->seek != NULL) {
			(void)mount->fs->seek(handle, 0, 2);
		}
	}
	vfs_os_unlock();
	if (result != 0) {
		return result;
	}
	return VFS_FD_BASE + (int)(file - files);
}

/**
 * @brief Read from an open descriptor.
 *
 * @param[in]  descriptor Descriptor from vfs_open().
 * @param[out] buffer     Destination buffer.
 * @param[in]  length     Maximum number of bytes to read.
 * @return The number of bytes read, or a negative errno.
 */
int vfs_read(int descriptor, void *buffer, size_t length)
{
	struct vfs_file *file;
	int result;

	vfs_os_lock();
	file = file_get(descriptor);
	result = file == NULL ? -EBADF : file->mount->fs->read(file->handle, buffer, length);
	vfs_os_unlock();
	return result;
}

/**
 * @brief Write to an open descriptor.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @param[in] buffer     Source buffer.
 * @param[in] length     Number of bytes to write.
 * @return The number of bytes written, -EBADF, -EROFS when the type has no write hook, or the
 *         type's error.
 */
int vfs_write(int descriptor, const void *buffer, size_t length)
{
	struct vfs_file *file;
	int result;

	vfs_os_lock();
	file = file_get(descriptor);
	if (file == NULL) {
		result = -EBADF;
	} else if (file->mount->fs->write == NULL) {
		result = -EROFS;
	} else {
		result = file->mount->fs->write(file->handle, buffer, length);
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Move the file position of an open descriptor.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @param[in] offset     Byte offset, interpreted according to @p whence.
 * @param[in] whence     0 from the start, 1 from the current position, 2 from the end.
 * @return The new position, -EBADF, -ENOTSUP when the type has no seek hook, or the type's
 *         error.
 */
int64_t vfs_lseek(int descriptor, int64_t offset, int whence)
{
	struct vfs_file *file;
	int64_t result;

	vfs_os_lock();
	file = file_get(descriptor);
	if (file == NULL) {
		result = -EBADF;
	} else if (file->mount->fs->seek == NULL) {
		result = -ENOTSUP;
	} else {
		result = file->mount->fs->seek(file->handle, offset, whence);
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Close a descriptor.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @return 0, the type's close error, or -EBADF.
 *
 * The slot is released and the mount's open_count decremented even when the close hook reports
 * an error, so a failing medium cannot leak descriptors.
 */
int vfs_close(int descriptor)
{
	struct vfs_file *file;
	int result;

	vfs_os_lock();
	file = file_get(descriptor);
	if (file == NULL) {
		result = -EBADF;
	} else {
		result = file->mount->fs->close != NULL ? file->mount->fs->close(file->handle) : 0;
		file->mount->open_count--;
		file->mount = NULL;
		file->handle = NULL;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Report the metadata of an open file.
 *
 * @param[in]  descriptor Descriptor from vfs_open().
 * @param[out] st         Structure receiving the metadata.
 * @return 0, -EBADF, -ENOTSUP when the type has no fstat hook, or the type's error.
 */
int vfs_fstat(int descriptor, struct vfs_stat *st)
{
	struct vfs_file *file;
	int result;

	vfs_os_lock();
	file = file_get(descriptor);
	if (file == NULL) {
		result = -EBADF;
	} else if (file->mount->fs->fstat == NULL) {
		result = -ENOTSUP;
	} else {
		result = file->mount->fs->fstat(file->handle, st);
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Resize an open file.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @param[in] size       New size in bytes.
 * @return 0, -EBADF, -ENOTSUP when the type has no truncate hook, or the type's error.
 */
int vfs_ftruncate(int descriptor, uint64_t size)
{
	struct vfs_file *file;
	int result;

	vfs_os_lock();
	file = file_get(descriptor);
	if (file == NULL) {
		result = -EBADF;
	} else if (file->mount->fs->truncate == NULL) {
		result = -ENOTSUP;
	} else {
		result = file->mount->fs->truncate(file->handle, size);
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Flush an open file to its medium.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @return 0, -EBADF, or the type's flush error.
 */
int vfs_fsync(int descriptor)
{
	struct vfs_file *file;
	int result;

	vfs_os_lock();
	file = file_get(descriptor);
	if (file == NULL) {
		result = -EBADF;
	} else {
		result = file->mount->fs->flush != NULL ? file->mount->fs->flush(file->handle) : 0;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Flush every file on one mount.
 *
 * @param[in] target Mount point.
 * @return 0, -ENOENT for an unknown target, or the type's sync error.
 */
int vfs_sync_mount(const char *target)
{
	char absolute[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result = normalize(target, absolute);

	vfs_os_lock();
	if (result != 0) {
		/* keep the error */
	} else if ((mount = mount_find_target(absolute)) == NULL) {
		result = -ENOENT;
	} else {
		result = mount->fs->sync != NULL ? mount->fs->sync(mount->priv) : 0;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Report one mount table entry by ordinal.
 *
 * @param[in]  index Ordinal among the live mounts, starting at zero.
 * @param[out] info  Filled with target, device name, type name and open count.
 * @return 0 on success, -ENOENT when @p index is past the last live mount.
 *
 * The slots are walked in order, counting only live ones. A nodev mount reports "none" as its
 * device name.
 */
int vfs_mount_get(uint32_t index, struct vfs_mount_info *info)
{
	int result = -ENOENT;

	vfs_os_lock();
	for (uint32_t slot = 0U, seen = 0U; slot < CONFIG_VFS_MAX_MOUNTS; slot++) {
		if (!mounts[slot].used || seen++ != index) {
			continue;
		}
		snprintf(info->target, sizeof(info->target), "%s", mounts[slot].target);
		snprintf(info->blk, sizeof(info->blk), "%s",
			mounts[slot].bdev != NULL ? bdev_name(mounts[slot].bdev) : "none");
		info->fstype = mounts[slot].fs->name;
		info->open_count = mounts[slot].open_count;
		result = 0;
		break;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Flush every mounted file system.
 *
 * @return 0 on success, or the first sync error encountered.
 *
 * All mounts are visited even after a failure, so one bad medium does not leave the others
 * dirty; only the first error is reported.
 */
int vfs_sync(void)
{
	int result = 0;

	vfs_os_lock();
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_MOUNTS; index++) {
		if (mounts[index].used && mounts[index].fs->sync != NULL) {
			int one = mounts[index].fs->sync(mounts[index].priv);

			if (result == 0) {
				result = one;
			}
		}
	}
	vfs_os_unlock();
	return result;
}

/* ---- paths ---- */

/**
 * @brief Report the metadata of a path.
 *
 * @param[in]  path Path in the VFS namespace.
 * @param[out] st   Structure receiving the metadata.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no stat hook, or the type's error.
 *
 * Under the lock: the root and any mount point itself report as a directory without consulting
 * a file system. Every other path is resolved and handed to the mount's stat hook.
 */
int vfs_stat(const char *path, struct vfs_stat *st)
{
	char absolute[VFS_PATH_MAX];
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result = normalize(path, absolute);

	vfs_os_lock();
	/* the root and a mount point itself answer without a file system */
	if (result != 0) {
		/* keep the error */
	} else if (strcmp(absolute, "/") == 0 || mount_find_target(absolute) != NULL) {
		memset(st, 0, sizeof(*st));
		st->dir = true;
		result = 0;
	} else if ((mount = mount_find_path(absolute, &path)) == NULL) {
		result = -ENOENT;
	} else if (mount->fs->stat == NULL) {
		result = -ENOTSUP;
	} else {
		snprintf(relative, sizeof(relative), "%s", path);
		result = mount->fs->stat(mount->priv, relative, st);
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Report the capacity of the file system holding a path.
 *
 * @param[in]  path       Any path inside the mounted file system.
 * @param[out] total      Receives the total size in bytes.
 * @param[out] free_bytes Receives the free space in bytes.
 * @return 0, -ENOENT, -ENOTSUP when the type has no statfs hook, or the type's error.
 */
int vfs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes)
{
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result;

	vfs_os_lock();
	result = resolve(path, &mount, relative);
	if (result == 0) {
		result = mount->fs->statfs != NULL ? mount->fs->statfs(mount->priv, total, free_bytes) : -ENOTSUP;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Resolve a path into its mount for the calls that change the namespace.
 *
 * @param[in]  path     Path to resolve.
 * @param[out] mount    Receives the covering mount.
 * @param[out] relative Buffer of VFS_PATH_MAX bytes receiving the in-mount path.
 * @return 0 on success, or a negative errno from resolve().
 *
 * It behaves exactly like resolve(); the separate name marks the call sites that create, remove
 * or rename entries, where the relative path must stay inside one mount.
 */
static int resolve_mount(const char *path, struct vfs_mount **mount, char relative[VFS_PATH_MAX])
{
	return resolve(path, mount, relative);
}

/**
 * @brief Create a directory.
 *
 * @param[in] path Path of the new directory.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no mkdir hook, or the type's error.
 */
int vfs_mkdir(const char *path)
{
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result;

	vfs_os_lock();
	result = resolve_mount(path, &mount, relative);
	if (result == 0) {
		result = mount->fs->mkdir != NULL ? mount->fs->mkdir(mount->priv, relative) : -ENOTSUP;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Remove a file.
 *
 * @param[in] path Path of the file.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no unlink hook, or the type's error.
 */
int vfs_unlink(const char *path)
{
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result;

	vfs_os_lock();
	result = resolve_mount(path, &mount, relative);
	if (result == 0) {
		result = mount->fs->unlink != NULL ? mount->fs->unlink(mount->priv, relative) : -ENOTSUP;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Remove an empty directory.
 *
 * @param[in] path Path of the directory.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no rmdir hook, or the type's error.
 */
int vfs_rmdir(const char *path)
{
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result;

	vfs_os_lock();
	result = resolve_mount(path, &mount, relative);
	if (result == 0) {
		result = mount->fs->rmdir != NULL ? mount->fs->rmdir(mount->priv, relative) : -ENOTSUP;
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Rename or move an entry.
 *
 * @param[in] from Existing path.
 * @param[in] to   New path.
 * @return 0, -ENOENT, -EXDEV when both paths are on different mounts, -ENOTSUP, or the type's
 *         error.
 *
 * Both paths are resolved under the lock; a rename never crosses a mount boundary.
 */
int vfs_rename(const char *from, const char *to)
{
	char relative_from[VFS_PATH_MAX];
	char relative_to[VFS_PATH_MAX];
	struct vfs_mount *mount_from;
	struct vfs_mount *mount_to;
	int result;

	vfs_os_lock();
	result = resolve_mount(from, &mount_from, relative_from);
	if (result == 0) {
		result = resolve_mount(to, &mount_to, relative_to);
	}
	if (result == 0 && mount_from != mount_to) {
		result = -EXDEV;
	}
	if (result == 0) {
		result = mount_from->fs->rename != NULL ?
			mount_from->fs->rename(mount_from->priv, relative_from, relative_to) : -ENOTSUP;
	}
	vfs_os_unlock();
	return result;
}

/* ---- directories ---- */

/**
 * @brief Open a directory.
 *
 * @param[in]  path  Directory path.
 * @param[out] error Receives 0 or the negative error; may be NULL.
 * @return The directory handle, or NULL on failure.
 *
 * Under the lock. "/" with nothing mounted there produces a heap-allocated synthetic directory
 * whose readdir() lists the mount points. Otherwise the type's opendir hook is called and its
 * handle wrapped in a heap object, so the handle can outlive the lock; if that allocation fails
 * the just-opened handle is closed again. The mount's open_count is bumped.
 */
struct vfs_dir *vfs_opendir(const char *path, int *error)
{
	char absolute[VFS_PATH_MAX];
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	struct vfs_dir *dir = NULL;
	void *handle = NULL;
	int result = normalize(path, absolute);

	vfs_os_lock();
	/* the synthetic root lists the mounts; every other path goes to a type */
	if (result != 0) {
		/* keep the error */
	} else if (strcmp(absolute, "/") == 0 && mount_find_target("/") == NULL) {
		dir = calloc(1, sizeof(*dir)); /* the list of mount points */
		result = dir == NULL ? -ENOMEM : 0;
	} else if ((mount = mount_find_path(absolute, &path)) == NULL) {
		result = -ENOENT;
	} else {
		snprintf(relative, sizeof(relative), "%s", path);
		result = mount->fs->opendir != NULL ? mount->fs->opendir(mount->priv, relative, &handle) :
			-ENOTSUP;
		if (result == 0) {
			dir = calloc(1, sizeof(*dir));
			if (dir == NULL) {
				if (mount->fs->closedir != NULL) {
					(void)mount->fs->closedir(handle);
				}
				result = -ENOMEM;
			} else {
				dir->mount = mount;
				dir->handle = handle;
				mount->open_count++;
			}
		}
	}
	vfs_os_unlock();
	if (error != NULL) {
		*error = dir != NULL ? 0 : result;
	}
	return dir;
}

/**
 * @brief Read the next entry of an open directory.
 *
 * @param[in]  dir   Directory from vfs_opendir().
 * @param[out] entry Entry filled with name, metadata and the directory flag.
 * @return 0 on success, -ENOENT at the end of the directory, or a negative errno.
 *
 * Synthetic directories walk the mount slots, skipping @c dir->index entries and naming each
 * target without its leading slash. Real directories delegate to the type's readdir hook.
 */
int vfs_readdir(struct vfs_dir *dir, struct vfs_dirent *entry)
{
	int result = -ENOENT;

	if (dir == NULL) {
		return -EINVAL;
	}
	vfs_os_lock();
	if (dir->mount != NULL) {
		result = dir->mount->fs->readdir(dir->handle, entry);
	} else {
		/* one directory entry per live mount, in slot order */
		for (uint32_t index = 0U, seen = 0U; index < CONFIG_VFS_MAX_MOUNTS; index++) {
			if (!mounts[index].used || seen++ != dir->index) {
				continue;
			}
			memset(entry, 0, sizeof(*entry));
			snprintf(entry->name, sizeof(entry->name), "%s", mounts[index].target + 1);
			entry->st.dir = true;
			dir->index++;
			result = 0;
			break;
		}
	}
	vfs_os_unlock();
	return result;
}

/**
 * @brief Close a directory and free its wrapper.
 *
 * @param[in] dir Directory from vfs_opendir(), may be NULL.
 *
 * The type's closedir hook runs and the mount's open_count drops under the lock; the wrapper
 * itself is freed afterwards, outside the lock.
 */
void vfs_closedir(struct vfs_dir *dir)
{
	if (dir == NULL) {
		return;
	}
	vfs_os_lock();
	if (dir->mount != NULL) {
		if (dir->mount->fs->closedir != NULL) {
			(void)dir->mount->fs->closedir(dir->handle);
		}
		dir->mount->open_count--;
	}
	vfs_os_unlock();
	free(dir);
}
