/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The virtual file system: the mount table, file descriptors and path resolution.
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


#ifndef CONFIG_VFS_MAX_MOUNTS
#define CONFIG_VFS_MAX_MOUNTS 8
#endif
#ifndef CONFIG_VFS_MAX_FILES
#define CONFIG_VFS_MAX_FILES 16
#endif
#ifndef CONFIG_VFS_MAX_FS_TYPES
#define CONFIG_VFS_MAX_FS_TYPES 8
#endif

#define TARGET_MAX 32

struct vfs_mount {
	bool used;
	char target[TARGET_MAX];
	struct block_device *bdev;
	const struct vfs_fs *fs;
	void *priv;
	uint32_t open_count;
};

struct vfs_file {
	struct vfs_mount *mount;
	void *handle;
};

struct vfs_dir {
	struct vfs_mount *mount; /* NULL: the list of mount points */
	void *handle;
	uint32_t index;
};

static const struct vfs_fs *fs_types[CONFIG_VFS_MAX_FS_TYPES];
static struct vfs_mount mounts[CONFIG_VFS_MAX_MOUNTS];
static struct vfs_file files[CONFIG_VFS_MAX_FILES];
static char cwd[VFS_PATH_MAX] = "/";

/* Sorted by target length, longest first: the deepest mount wins. */
static struct vfs_mount *mounts_by_target[CONFIG_VFS_MAX_MOUNTS];
static uint32_t mount_count;

/* Weak defaults: the host tests and the target provide the real lock. */
__attribute__((weak)) void vfs_os_lock(void) {}
__attribute__((weak)) void vfs_os_unlock(void) {}

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

static struct vfs_mount *mount_find_target(const char *target)
{
	for (uint32_t index = 0U; index < CONFIG_VFS_MAX_MOUNTS; index++) {
		if (mounts[index].used && strcmp(mounts[index].target, target) == 0) {
			return &mounts[index];
		}
	}
	return NULL;
}

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
	if (result == 0 && bdev != NULL) {
		result = bdev_claim_mount(bdev);
		claimed = result == 0;
	}
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

int vfs_mount(const char *blk_name, const char *target, const char *fstype)
{
	return vfs_mount_ex(blk_name, target, fstype, 0U);
}

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

static void (*format_progress)(void *context, uint64_t done, uint64_t total);
static void *format_progress_context;
static bool format_active;
static uint64_t format_highest;
static uint64_t format_written;
static uint64_t format_expected;
static struct block_device *format_device;

void vfs_set_format_progress(void (*callback)(void *context, uint64_t done, uint64_t total), void *context)
{
	format_progress = callback;
	format_progress_context = context;
}

void vfs_format_expect(struct block_device *bdev, uint64_t sectors)
{
	if (!format_active || bdev != format_device || sectors == 0U) {
		return;
	}
	format_expected = sectors;
}

/*
 * A FAT format writes only the reserved area, the FATs and the root directory, all near the
 * start of the card: measuring progress by the sector position would sit at 0% and then jump to
 * 100%. The formatter instead estimates the sectors it will write and this counts them.
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

int vfs_stat(const char *path, struct vfs_stat *st)
{
	char absolute[VFS_PATH_MAX];
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	int result = normalize(path, absolute);

	vfs_os_lock();
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

/* Resolve @p path and give the mount and the path inside it; for the mutating calls. */
static int resolve_mount(const char *path, struct vfs_mount **mount, char relative[VFS_PATH_MAX])
{
	return resolve(path, mount, relative);
}

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

struct vfs_dir *vfs_opendir(const char *path, int *error)
{
	char absolute[VFS_PATH_MAX];
	char relative[VFS_PATH_MAX];
	struct vfs_mount *mount;
	struct vfs_dir *dir = NULL;
	void *handle = NULL;
	int result = normalize(path, absolute);

	vfs_os_lock();
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
