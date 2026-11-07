/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file vfs.h
 * @brief Virtual file system: block devices, mount points, one name space.
 *
 * A file system type (FAT, ext) is mounted from a block device at a mount point "/name"; paths
 * then go through the mount table to the file system. Absolute paths start at the root, "/"
 * lists the mount points when nothing is mounted there. File descriptors are numbers from
 * VFS_FD_BASE up; the C library's open/read/fopen use them. All calls return a negative errno
 * on failure.
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

/** @brief First file descriptor number; 0..2 are the console. */
#define VFS_FD_BASE  3
/** @brief Longest file name, excluding the terminating zero. */
#define VFS_NAME_MAX 255
/** @brief Longest path, including the terminating zero. */
#define VFS_PATH_MAX 512
/** @brief Longest block device name in a mount description. */
#define VFS_BLK_NAME 32

/** @brief Open for reading. */
#define VFS_O_READ   0x01
/** @brief Open for writing. */
#define VFS_O_WRITE  0x02
/** @brief Make the file when it does not exist. */
#define VFS_O_CREATE 0x04
/** @brief With VFS_O_CREATE: fail when it exists. */
#define VFS_O_EXCL   0x08
/** @brief With VFS_O_WRITE: drop the contents. */
#define VFS_O_TRUNC  0x10
/** @brief Writes go to the end. */
#define VFS_O_APPEND 0x20

/** @brief File system type flag: the type needs no block device. */
#define VFS_FS_NODEV	 0x1
/** @brief File system type flag: every volume of this type is read only. */
#define VFS_FS_READ_ONLY 0x2
/** @brief Mount flag: mount read only, passed to the type's mount hook. */
#define VFS_MOUNT_READ_ONLY 0x1

/**
 * @brief Metadata of a file or directory.
 */
struct vfs_stat {
	uint64_t size; /**< Size in bytes. */
	bool dir; /**< Whether the entry is a directory. */
	bool read_only; /**< Whether the entry cannot be changed. */
	uint16_t date; /**< Date of the last write, DOS format (0 when not known). */
	uint16_t time; /**< Time of the last write, DOS format (0 when not known). */
};

/**
 * @brief One directory entry as returned by vfs_readdir().
 */
struct vfs_dirent {
	char name[VFS_NAME_MAX + 1]; /**< Entry name, NUL terminated. */
	struct vfs_stat st; /**< Metadata of the entry. */
};

/**
 * @brief Description of a volume found by vfs_probe().
 */
struct vfs_volume_info {
	char fstype[16]; /**< File system type name. */
	char label[BDEV_LABEL_LEN]; /**< Volume label. */
	uint64_t total_bytes; /**< Total capacity in bytes. */
	uint64_t free_bytes; /**< Free space in bytes. */
	bool read_only; /**< Whether the volume is mounted read only. */
};

/**
 * @brief Operations a file system type provides to the VFS.
 *
 * The mount hook receives a private pointer that every later hook gets back. Hooks that a type
 * does not implement are NULL, and calls that need them fail with -ENOTSUP.
 */
struct vfs_fs {
	const char *name; /**< Type name, as used on the mount command line. */
	uint32_t flags; /**< VFS_FS_* flags. */
	int (*mount)(struct block_device *bdev, uint32_t flags, void **priv); /**< Mount a volume. */
	int (*umount)(void *priv); /**< Unmount a volume. */
	int (*sync)(void *priv); /**< Flush a mounted volume. */
	int (*format)(struct block_device *bdev, const char *label, uint32_t arg); /**< Format. */
	int (*open)(void *priv, const char *path, uint32_t flags, void **file); /**< Open a file. */
	int (*read)(void *file, void *buffer, size_t length); /**< Read a file. */
	int (*write)(void *file, const void *buffer, size_t length); /**< Write a file. */
	int64_t (*seek)(void *file, int64_t offset, int whence); /**< Move a file position. */
	int (*truncate)(void *file, uint64_t size); /**< Resize a file. */
	int (*flush)(void *file); /**< Flush a file. */
	int (*close)(void *file); /**< Close a file. */
	int (*fstat)(void *file, struct vfs_stat *st); /**< Stat an open file. */
	int (*stat)(void *priv, const char *path, struct vfs_stat *st); /**< Stat a path. */
	int (*mkdir)(void *priv, const char *path); /**< Create a directory. */
	int (*unlink)(void *priv, const char *path); /**< Remove a file. */
	int (*rmdir)(void *priv, const char *path); /**< Remove a directory. */
	int (*rename)(void *priv, const char *from, const char *to); /**< Rename an entry. */
	int (*opendir)(void *priv, const char *path, void **dir); /**< Open a directory. */
	int (*readdir)(void *dir, struct vfs_dirent *entry); /**< Read a directory entry. */
	int (*closedir)(void *dir); /**< Close a directory. */
	int (*statfs)(void *priv, uint64_t *total, uint64_t *free_bytes); /**< Volume capacity. */
	int (*volume_info)(void *priv, struct vfs_volume_info *info); /**< Volume description. */
};

/**
 * @brief Add a file system type to the VFS.
 *
 * @details Registration is idempotent: an already registered pointer returns 0. The lock is
 * held while the type table changes.
 *
 * @param[in] fs Descriptor; @c name and @c mount must be set.
 * @return 0 on success, -EINVAL for a missing mandatory field, or -ENOMEM when the type table
 * is full.
 */
int vfs_register(const struct vfs_fs *fs);
/**
 * @brief Remove a file system type from the VFS.
 *
 * @details The table is compacted over the hole, because lookups stop at the first NULL slot.
 *
 * @param[in] fs Descriptor previously passed to vfs_register().
 * @return 0 on success, or -ENOENT when @p fs is not registered.
 */
int vfs_unregister(const struct vfs_fs *fs);

/**
 * @brief Mount a block device or a nodev file system at a target with default flags.
 *
 * @param[in] blk    Block device name, or NULL for a nodev file system.
 * @param[in] target Mount point; one level only.
 * @param[in] fstype File system name, or NULL to try every type.
 * @return 0 on success, or the negative errno from vfs_mount_ex().
 */
int vfs_mount(const char *blk, const char *target, const char *fstype);
/**
 * @brief Mount a block device or a nodev file system at a target.
 *
 * @details Under the VFS lock: the target is normalised and rejected if deeper than one level
 * or too long. A named type is looked up; unless it is a nodev type, the block device is opened
 * (512-byte sectors only), a duplicate target is refused, and the device is claimed so a second
 * mount cannot take it. A free slot is reserved, the mount hook runs, and a successful mount is
 * recorded and sorted into the lookup table. Any failure releases the claim and closes the
 * device.
 *
 * @param[in] blk    Block device name, NULL for file systems flagged VFS_FS_NODEV.
 * @param[in] target Mount point; one level only.
 * @param[in] fstype File system name, or NULL to probe every device-backed type.
 * @param[in] flags  VFS_MOUNT_* flags passed to the type's mount hook.
 * @return 0 on success, -EINVAL, -ENAMETOOLONG, -ENODEV, -EBUSY, -ENOMEM, or the error from
 * the file system's mount hook.
 */
int vfs_mount_ex(const char *blk, const char *target, const char *fstype, uint32_t flags);
/**
 * @brief Unmount the file system at a target.
 *
 * @details Under the lock: validates the target, refuses while files or directories are open
 * below it, runs the umount hook, releases the device claim and closes it, then frees the slot.
 *
 * @param[in] target Mount point.
 * @return 0 on success, -EINVAL for an invalid or unknown target, -EBUSY while files or
 * directories are open below it, or the type's umount error.
 */
int vfs_umount(const char *target);

/**
 * @brief Make a new file system on an unmounted block device.
 *
 * @details A format is minutes of card I/O, so the VFS lock is not taken; the block device is
 * opened and claimed instead, which keeps mounts out. The format progress state is armed
 * around the type's format hook. The old contents are lost.
 *
 * @param[in] blk    Block device name.
 * @param[in] fstype File system name, or NULL for the first registered type.
 * @param[in] label  Volume label, may be NULL.
 * @param[in] arg    Type-specific argument: allocation unit, or the ext2/ext3/ext4 choice.
 * @return 0 on success, -ENODEV, -ENOTSUP when the type cannot format, or the formatter's
 * error.
 */
int vfs_format(const char *blk, const char *fstype, const char *label, uint32_t arg);
/**
 * @brief Observe vfs_format() progress.
 *
 * @details The callback gets the highest reported sector and the device size, from the thread
 * that formats. This is an approximate position indicator.
 *
 * @param[in] callback Progress function, or NULL to disable reporting.
 * @param[in] context  Opaque pointer passed back to @p callback.
 */
void vfs_set_format_progress(void (*callback)(void *context, uint64_t done, uint64_t total), void *context);
/**
 * @brief Declare how many sectors the running format will write.
 *
 * @details For file system drivers. A formatter announces how many sectors it is about to
 * write, then reports every write; the value is ignored unless a format of @p bdev is in
 * progress.
 *
 * @param[in] bdev    Device being formatted.
 * @param[in] sectors Expected sector count; ignored when zero.
 */
void vfs_format_expect(struct block_device *bdev, uint64_t sectors);
/**
 * @brief Report sectors written by a formatter.
 *
 * @details For file system drivers. Updates the approximate progress with a high-water mark
 * and does nothing outside vfs_format().
 *
 * @param[in] bdev    Device being formatted.
 * @param[in] sectors Number of sectors just written.
 */
void vfs_format_report_write(struct block_device *bdev, uint64_t sectors);

/**
 * @brief Probe an unmounted block device.
 *
 * @details The device is opened and claimed, then each registered device-backed type is tried
 * with a read-only mount until one succeeds; the probe mounts the volume briefly, reads its
 * description, and unmounts it again.
 *
 * @param[in]  blk  Device name.
 * @param[out] info Receives type, label, sizes and the read-only flag.
 * @return 0 on success, -EINVAL, -ENODEV, -EBUSY when the device is in use, or the error from
 * the file system.
 */
int vfs_probe(const char *blk, struct vfs_volume_info *info);

/**
 * @brief Open a file and return a VFS descriptor.
 *
 * @details Under the lock: resolves the path, calls the type's open hook, then takes the first
 * free file slot. With every slot busy the handle is closed again and -EMFILE returned.
 * VFS_O_APPEND seeks to the end once here, so later writes append.
 *
 * @param[in] path  Path in the VFS namespace.
 * @param[in] flags VFS_O_* access and creation flags.
 * @return A descriptor at or above VFS_FD_BASE, or a negative errno.
 */
int vfs_open(const char *path, uint32_t flags);
/**
 * @brief Read from an open descriptor.
 *
 * @param[in]  descriptor Descriptor from vfs_open().
 * @param[out] buffer     Destination buffer.
 * @param[in]  length     Maximum number of bytes to read.
 * @return The number of bytes read, or a negative errno.
 */
int vfs_read(int descriptor, void *buffer, size_t length);
/**
 * @brief Write to an open descriptor.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @param[in] buffer     Source buffer.
 * @param[in] length     Number of bytes to write.
 * @return The number of bytes written, -EBADF, -EROFS when the type has no write hook, or the
 * type's error.
 */
int vfs_write(int descriptor, const void *buffer, size_t length);
/**
 * @brief Move the file position of an open descriptor.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @param[in] offset     Byte offset, interpreted according to @p whence.
 * @param[in] whence     0 from the start, 1 from the current position, 2 from the end.
 * @return The new position, -EBADF, -ENOTSUP when the type has no seek hook, or the type's
 * error.
 */
int64_t vfs_lseek(int descriptor, int64_t offset, int whence);
/**
 * @brief Close a descriptor.
 *
 * @details The slot is released and the mount's open count decremented even when the close
 * hook reports an error, so a failing medium cannot leak descriptors.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @return 0, the type's close error, or -EBADF.
 */
int vfs_close(int descriptor);
/**
 * @brief Report the metadata of an open file.
 *
 * @param[in]  descriptor Descriptor from vfs_open().
 * @param[out] st         Receives the metadata.
 * @return 0, -EBADF, -ENOTSUP when the type has no fstat hook, or the type's error.
 */
int vfs_fstat(int descriptor, struct vfs_stat *st);
/**
 * @brief Resize an open file.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @param[in] size       New size in bytes.
 * @return 0, -EBADF, -ENOTSUP when the type has no truncate hook, or the type's error.
 */
int vfs_ftruncate(int descriptor, uint64_t size);
/**
 * @brief Flush an open file to its medium.
 *
 * @param[in] descriptor Descriptor from vfs_open().
 * @return 0, -EBADF, or the type's flush error.
 */
int vfs_fsync(int descriptor);
/**
 * @brief Flush every file on one mount.
 *
 * @param[in] target Mount point.
 * @return 0, -ENOENT for an unknown target, or the type's sync error.
 */
int vfs_sync_mount(const char *target);

/**
 * @brief Report the metadata of a path.
 *
 * @details The root and any mount point itself report as a directory without consulting a file
 * system; every other path is resolved and handed to the mount's stat hook.
 *
 * @param[in]  path Path in the VFS namespace.
 * @param[out] st   Receives the metadata.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no stat hook, or the type's error.
 */
int vfs_stat(const char *path, struct vfs_stat *st);
/**
 * @brief Report the capacity of the file system holding a path.
 *
 * @param[in]  path       Any path inside the mounted file system.
 * @param[out] total      Receives the total size in bytes.
 * @param[out] free_bytes Receives the free space in bytes.
 * @return 0, -ENOENT, -ENOTSUP when the type has no statfs hook, or the type's error.
 */
int vfs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes);
/**
 * @brief Create a directory.
 *
 * @param[in] path Path of the new directory.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no mkdir hook, or the type's error.
 */
int vfs_mkdir(const char *path);
/**
 * @brief Remove a file.
 *
 * @param[in] path Path of the file.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no unlink hook, or the type's error.
 */
int vfs_unlink(const char *path);
/**
 * @brief Remove an empty directory.
 *
 * @param[in] path Path of the directory.
 * @return 0, -ENOENT, -ENOTSUP when the mount has no rmdir hook, or the type's error.
 */
int vfs_rmdir(const char *path);
/**
 * @brief Rename or move an entry.
 *
 * @details Both paths are resolved under the lock; a rename never crosses a mount boundary.
 *
 * @param[in] from Existing path.
 * @param[in] to   New path.
 * @return 0, -ENOENT, -EXDEV when both paths are on different mounts, -ENOTSUP, or the type's
 * error.
 */
int vfs_rename(const char *from, const char *to);

/** @brief An open directory handle (defined in the implementation). */
struct vfs_dir;
/**
 * @brief Open a directory.
 *
 * @details "/" with nothing mounted there produces a synthetic directory whose readdir() lists
 * the mount points. Otherwise the type's opendir hook runs and its handle is wrapped in a heap
 * object so it can outlive the lock.
 *
 * @param[in]  path  Directory path.
 * @param[out] error Receives 0 or the negative error; may be NULL.
 * @return The directory handle, or NULL on failure.
 */
struct vfs_dir *vfs_opendir(const char *path, int *error);
/**
 * @brief Read the next entry of an open directory.
 *
 * @param[in]  dir   Directory from vfs_opendir().
 * @param[out] entry Receives name, metadata and the directory flag.
 * @return 0 on success, -ENOENT at the end of the directory, or a negative errno.
 */
int vfs_readdir(struct vfs_dir *dir, struct vfs_dirent *entry);
/**
 * @brief Close a directory and free its wrapper.
 *
 * @param[in] dir Directory from vfs_opendir(); NULL is ignored.
 */
void vfs_closedir(struct vfs_dir *dir);

/**
 * @brief Write everything out on every mount.
 *
 * @details All mounts are visited even after a failure, so one bad medium does not leave the
 * others dirty; only the first error is reported.
 *
 * @return 0 on success, or the first sync error encountered.
 */
int vfs_sync(void);

/**
 * @brief One entry of the mount table.
 */
struct vfs_mount_info {
	char target[VFS_NAME_MAX + 1]; /**< Mount point. */
	char blk[VFS_BLK_NAME]; /**< Block device name, or "none" for a nodev mount. */
	const char *fstype; /**< File system type name. */
	uint32_t open_count; /**< Files and directories open below the mount. */
};
/**
 * @brief Report one mount table entry by ordinal.
 *
 * @details The slots are walked in order, counting only live mounts.
 *
 * @param[in]  index Ordinal among the live mounts, starting at zero.
 * @param[out] info  Receives target, device name, type name and open count.
 * @return 0 on success, or -ENOENT after the last one.
 */
int vfs_mount_get(uint32_t index, struct vfs_mount_info *info);

/* ---- the platform lock and the file system types ---- */

/** @brief Take the recursive VFS lock, waiting without a bound. */
void vfs_os_lock(void);
/** @brief Release the recursive VFS lock. */
void vfs_os_unlock(void);

/**
 * @brief Register the FatFs types: fat, fat32, exfat.
 *
 * @return 0 on success, or the negative errno from vfs_register().
 */
int vfs_ff_register(void);
/**
 * @brief Register the ext types: ext2, ext3, ext4.
 *
 * @return 0 on success, or the negative errno from vfs_register().
 */
int vfs_ext_register(void);

#endif /* CARDSIGHT_VFS_H */
