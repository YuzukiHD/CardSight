/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Virtual file system: block devices, mount points, one name space.
 *
 * A file system type (FAT, ext) is mounted from a block device at a mount point "/name"; paths
 * then go through the mount table to the file system. Absolute paths start at the root, "/" lists
 * the mount points when nothing is mounted there. File descriptors are numbers from VFS_FD_BASE
 * up; the C library's open/read/fopen use them. All calls return a negative errno on failure.
 *
 * One lock serialises the calls except the long ones: a format and a probe claim the device
 * instead, so they do not stop every other call.
 */

#ifndef CARDSIGHT_VFS_H
#define CARDSIGHT_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hal/blkdev.h>

#define VFS_FD_BASE  3 /* 0..2 are the console */
#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 512
#define VFS_BLK_NAME 32

#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_CREATE 0x04 /* make the file when it does not exist */
#define VFS_O_EXCL   0x08 /* with VFS_O_CREATE: fail when it exists */
#define VFS_O_TRUNC  0x10 /* with VFS_O_WRITE: drop the contents */
#define VFS_O_APPEND 0x20 /* writes go to the end */

#define VFS_FS_NODEV	 0x1 /* the type needs no block device */
#define VFS_FS_READ_ONLY 0x2 /* every volume of this type is read only */
#define VFS_MOUNT_READ_ONLY 0x1

struct vfs_stat {
	uint64_t size;
	bool dir;
	bool read_only;
	uint16_t date; /* of the last write, DOS format (0 when not known) */
	uint16_t time;
};

struct vfs_dirent {
	char name[VFS_NAME_MAX + 1];
	struct vfs_stat st;
};

struct vfs_volume_info {
	char fstype[16];
	char label[BDEV_LABEL_LEN];
	uint64_t total_bytes;
	uint64_t free_bytes;
	bool read_only;
};

struct vfs_fs {
	const char *name;
	unsigned int flags;
	int (*mount)(struct block_device *bdev, unsigned int flags, void **priv);
	int (*umount)(void *priv);
	int (*sync)(void *priv);
	int (*format)(struct block_device *bdev, const char *label, unsigned int arg);
	int (*open)(void *priv, const char *path, unsigned int flags, void **file);
	int (*read)(void *file, void *buffer, size_t length);
	int (*write)(void *file, const void *buffer, size_t length);
	int64_t (*seek)(void *file, int64_t offset, int whence);
	int (*truncate)(void *file, uint64_t size);
	int (*flush)(void *file);
	int (*close)(void *file);
	int (*fstat)(void *file, struct vfs_stat *st);
	int (*stat)(void *priv, const char *path, struct vfs_stat *st);
	int (*mkdir)(void *priv, const char *path);
	int (*unlink)(void *priv, const char *path);
	int (*rmdir)(void *priv, const char *path);
	int (*rename)(void *priv, const char *from, const char *to);
	int (*opendir)(void *priv, const char *path, void **dir);
	int (*readdir)(void *dir, struct vfs_dirent *entry);
	int (*closedir)(void *dir);
	int (*statfs)(void *priv, uint64_t *total, uint64_t *free_bytes);
	int (*volume_info)(void *priv, struct vfs_volume_info *info);
};

int vfs_register(const struct vfs_fs *fs);
int vfs_unregister(const struct vfs_fs *fs);

int vfs_mount(const char *blk, const char *target, const char *fstype);
int vfs_mount_ex(const char *blk, const char *target, const char *fstype, unsigned int flags);
int vfs_umount(const char *target);

/** Make a new file system on an unmounted block device; the old contents are lost. */
int vfs_format(const char *blk, const char *fstype, const char *label, unsigned int arg);
/**
 * Observe vfs_format(): the callback gets the highest reported sector and the device size, from
 * the thread that formats. This is an approximate position indicator.
 */
void vfs_set_format_progress(void (*callback)(void *context, uint64_t done, uint64_t total), void *context);
/** For file system drivers: report the end of a write to @p bdev; does nothing outside vfs_format(). */
void vfs_format_report(struct block_device *bdev, uint64_t done, uint64_t total);

/** Probe an unmounted block device by mounting it briefly, then unmounting it. */
int vfs_probe(const char *blk, struct vfs_volume_info *info);

int vfs_open(const char *path, unsigned int flags);
int vfs_read(int descriptor, void *buffer, size_t length);
int vfs_write(int descriptor, const void *buffer, size_t length);
int64_t vfs_lseek(int descriptor, int64_t offset, int whence);
int vfs_close(int descriptor);
int vfs_fstat(int descriptor, struct vfs_stat *st);
int vfs_ftruncate(int descriptor, uint64_t size);
int vfs_fsync(int descriptor);
int vfs_sync_mount(const char *target);

int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *from, const char *to);

struct vfs_dir;
struct vfs_dir *vfs_opendir(const char *path, int *error);
int vfs_readdir(struct vfs_dir *dir, struct vfs_dirent *entry);
void vfs_closedir(struct vfs_dir *dir);

/** Write everything out on every mount. */
int vfs_sync(void);

struct vfs_mount_info {
	char target[VFS_NAME_MAX + 1];
	char blk[VFS_BLK_NAME];
	const char *fstype;
	unsigned int open_count;
};
/** The @p index-th mount, 0 or -ENOENT after the last one. */
int vfs_mount_get(unsigned int index, struct vfs_mount_info *info);

/* ---- the platform lock and the file system types ---- */

void vfs_os_lock(void);
void vfs_os_unlock(void);

int vfs_ff_register(void); /* FatFs: fat, fat32, exfat */
int vfs_ext_register(void); /* ext2/3/4 */

#endif /* CARDSIGHT_VFS_H */
