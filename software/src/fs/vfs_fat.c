/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file vfs_fat.c
 * @brief FatFs as a VFS file system type: FAT12/16/32 and exFAT.
 *
 * A mount takes one of the FF_VOLUMES drive numbers ("N:"); the disk layer moves data through a
 * 64-byte aligned bounce buffer per drive and direction, because the block layer only does DMA
 * on aligned buffers and a drive backed by another file system does its I/O inside the outer
 * call (the storage proxy) - a shared buffer would be overwritten under it.
 */

#define LOG_MODULE "fatfs"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <init.h>
#include <log.h>
#include <vfs.h>

#include "ff.h"
#include "diskio.h"


/** @brief Sectors copied per bounce-buffer round trip. */
#define BOUNCE_SECTORS 16

/** @brief Device behind each FatFs drive number; NULL marks a free drive. */
static struct block_device *drive[FF_VOLUMES];
/** @brief Read bounce buffers, one per drive because a nested file system may re-enter. */
static uint8_t bounce_read[FF_VOLUMES][BOUNCE_SECTORS * 512] __attribute__((aligned(64)));
/** @brief Write bounce buffers, one per drive and direction. */
static uint8_t bounce_write[FF_VOLUMES][BOUNCE_SECTORS * 512] __attribute__((aligned(64)));

/* ---- the FatFs disk layer ---- */

/**
 * @brief Report the state of a FatFs drive.
 *
 * @param[in] pdrv Drive number.
 * @return STA_NOINIT when the drive has no device, STA_PROTECT when it is read-only, else 0.
 */
DSTATUS disk_status(BYTE pdrv)
{
	struct block_device *bdev = pdrv < FF_VOLUMES ? drive[pdrv] : NULL;

	if (bdev == NULL) {
		return STA_NOINIT;
	}
	return bdev_read_only(bdev) ? STA_PROTECT : 0;
}

/**
 * @brief Initialise a FatFs drive.
 *
 * @param[in] pdrv Drive number.
 * @return The result of disk_status(); the device is already open in the block layer.
 */
DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

/**
 * @brief Read sectors for FatFs.
 *
 * @param[in]  pdrv   Drive number.
 * @param[out] buffer Destination buffer.
 * @param[in]  sector First sector.
 * @param[in]  count  Number of sectors.
 * @return RES_OK, RES_NOTRDY, or RES_ERROR.
 *
 * The block layer only DMAs into 64-byte aligned buffers, so an unaligned request is copied
 * through the drive's read bounce buffer in chunks of BOUNCE_SECTORS.
 */
DRESULT disk_read(BYTE pdrv, BYTE *buffer, LBA_t sector, UINT count)
{
	struct block_device *bdev = pdrv < FF_VOLUMES ? drive[pdrv] : NULL;

	if (bdev == NULL) {
		return RES_NOTRDY;
	}
	/* an aligned buffer can go to the DMA-capable block layer directly */
	if (((uintptr_t)buffer & 63U) == 0U) {
		return bdev_read(bdev, sector, buffer, count) != 0 ? RES_ERROR : RES_OK;
	}
	while (count != 0U) {
		UINT chunk = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;

		if (bdev_read(bdev, sector, bounce_read[pdrv], chunk) != 0) {
			return RES_ERROR;
		}
		memcpy(buffer, bounce_read[pdrv], (size_t)chunk * 512U);
		buffer += chunk * 512U;
		sector += chunk;
		count -= chunk;
	}
	return RES_OK;
}

/**
 * @brief Write sectors for FatFs.
 *
 * @param[in] pdrv   Drive number.
 * @param[in] buffer Source buffer.
 * @param[in] sector First sector.
 * @param[in] count  Number of sectors.
 * @return RES_OK, RES_NOTRDY, or RES_ERROR.
 *
 * The write is reported to the format progress counter first. An aligned source goes straight
 * to the block layer; anything else is copied through the drive's write bounce buffer, which is
 * separate from the read one so a nested proxy read cannot overwrite data still to be written.
 */
DRESULT disk_write(BYTE pdrv, const BYTE *buffer, LBA_t sector, UINT count)
{
	struct block_device *bdev = pdrv < FF_VOLUMES ? drive[pdrv] : NULL;

	if (bdev == NULL) {
		return RES_NOTRDY;
	}
	vfs_format_report_write(bdev, count);
	/* the write direction has its own buffer: a proxy read cannot overwrite it */
	if (((uintptr_t)buffer & 63U) == 0U) {
		return bdev_write(bdev, sector, buffer, count) != 0 ? RES_ERROR : RES_OK;
	}
	while (count != 0U) {
		UINT chunk = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;

		memcpy(bounce_write[pdrv], buffer, (size_t)chunk * 512U);
		if (bdev_write(bdev, sector, bounce_write[pdrv], chunk) != 0) {
			return RES_ERROR;
		}
		buffer += chunk * 512U;
		sector += chunk;
		count -= chunk;
	}
	return RES_OK;
}

/**
 * @brief Answer the FatFs disk ioctls.
 *
 * @param[in] pdrv    Drive number.
 * @param[in] command CTRL_SYNC, GET_SECTOR_COUNT, or GET_BLOCK_SIZE.
 * @param[in,out] buffer Output for the two size commands; unused for CTRL_SYNC.
 * @return RES_OK, RES_NOTRDY, or RES_PARERR for an unknown command.
 *
 * The erase block is reported as 128 sectors, the granularity f_mkfs() uses for alignment.
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
	struct block_device *bdev = pdrv < FF_VOLUMES ? drive[pdrv] : NULL;

	if (bdev == NULL) {
		return RES_NOTRDY;
	}
	switch (command) {
	case CTRL_SYNC:
		return bdev_flush(bdev) != 0 ? RES_ERROR : RES_OK;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buffer = bdev_nr_sectors(bdev);
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buffer = 128U;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}

/* ---- the file system type ---- */

/**
 * @brief Per-mount state of the FatFs type.
 */
struct fat_priv {
	FATFS fs;		/**< Library volume object. */
	BYTE drv;		/**< FatFs drive number. */
	bool read_only;		/**< Volume was mounted or formatted read-only. */
	char root[4];		/**< Drive name passed to every library call, "N:". */
};

/**
 * @brief An open FAT file: the library object.
 */
struct fat_file {
	FIL file;		/**< Library file object. */
};

/**
 * @brief An open FAT directory: the library object and its reusable FILINFO.
 */
struct fat_dir {
	DIR dir;		/**< Library directory object. */
	FILINFO info;		/**< Latest entry, reused by f_readdir(). */
};

/**
 * @brief Translate a FatFs result into a negated errno.
 *
 * @param[in] result FatFs status.
 * @return 0 for FR_OK, otherwise the matching negative errno; unknown errors become -EIO.
 */
static int err_of(FRESULT result)
{
	switch (result) {
	case FR_OK: return 0;
	case FR_DISK_ERR:
	case FR_INT_ERR: return -EIO;
	case FR_NOT_READY: return -ENODEV;
	case FR_NO_FILE:
	case FR_NO_PATH: return -ENOENT;
	case FR_INVALID_NAME: return -EINVAL;
	case FR_DENIED: return -EACCES;
	case FR_EXIST: return -EEXIST;
	case FR_INVALID_OBJECT:
	case FR_INVALID_PARAMETER: return -EINVAL;
	case FR_WRITE_PROTECTED: return -EROFS;
	case FR_INVALID_DRIVE: return -ENODEV;
	case FR_NOT_ENABLED:
	case FR_NO_FILESYSTEM: return -EINVAL;
	case FR_MKFS_ABORTED: return -ENOSPC;
	case FR_TIMEOUT: return -ETIMEDOUT;
	case FR_LOCKED: return -EBUSY;
	case FR_NOT_ENOUGH_CORE: return -ENOMEM;
	case FR_TOO_MANY_OPEN_FILES: return -EMFILE;
	default: return -EIO;
	}
}

/**
 * @brief Bind a device to a free FatFs drive number.
 *
 * @param[in] bdev Device to bind.
 * @return The drive number, or -EMFILE when all drives are taken.
 *
 * The drive number is what FatFs uses as its volume index; the VFS root name is built from it.
 */
static int drive_take(struct block_device *bdev)
{
	for (int index = 0; index < FF_VOLUMES; index++) {
		if (drive[index] == NULL) {
			drive[index] = bdev;
			return index;
		}
	}
	return -EMFILE;
}

/* "N:" + the path inside the file system */
/**
 * @brief Prefix a relative path with the mount's drive name.
 *
 * @param[in]  priv     Mount state; its @c root holds "N:".
 * @param[in]  relative Path inside the file system.
 * @param[out] output   Buffer receiving "N:" plus the path.
 * @param[in]  size     Size of @p output in bytes.
 * @return 0 on success, -ENAMETOOLONG when the result does not fit.
 */
static int full_path(const struct fat_priv *priv, const char *relative, char *output, size_t size)
{
	size_t length = strlen(relative);

	if (length + 3U > size) {
		return -ENAMETOOLONG;
	}
	memcpy(output, priv->root, 2U);
	memcpy(output + 2U, relative, length + 1U);
	return 0;
}

/**
 * @brief Declare a full "N:<path>" buffer and fill it, returning on failure.
 *
 * @param[in]  priv     Mount state.
 * @param[in]  relative Path inside the file system.
 * @param[out] variable Name of the array to declare and fill.
 *
 * The array has room for VFS_PATH_MAX plus the two-character drive prefix and the terminator.
 * The statement returns the negative errno from full_path() when the path is too long.
 */
#define PATH(priv, relative, variable)                                                             \
	char variable[VFS_PATH_MAX + 4];                                                          \
	do {                                                                                      \
		int _result = full_path((priv), (relative), variable, sizeof(variable));          \
		if (_result != 0) {                                                               \
			return _result;                                                           \
		}                                                                                 \
	} while (0)

/**
 * @brief Mount a FAT, FAT32, or exFAT volume.
 *
 * @param[in]  bdev  Device holding the volume.
 * @param[in]  flags VFS_MOUNT_* flags; VFS_MOUNT_READ_ONLY forces a read-only mount.
 * @param[out] priv  Receives the allocated mount state.
 * @return 0 on success, -ENOMEM, -EMFILE when no drive is free, or the mapped FatFs error.
 *
 * A drive number is taken first, the mount state records the read-only decision and its drive
 * name, and f_mount() attaches the volume. On failure the drive is released and the state
 * freed; the error is also logged with the device name.
 */
static int fat_mount(struct block_device *bdev, uint32_t flags, void **priv)
{
	struct fat_priv *fat = calloc(1, sizeof(*fat));
	int slot;
	int result;

	if (fat == NULL) {
		return -ENOMEM;
	}
	/* a free drive number is the volume index FatFs uses */
	slot = drive_take(bdev);
	if (slot < 0) {
		free(fat);
		return slot;
	}
	fat->drv = (BYTE)slot;
	fat->read_only = bdev_read_only(bdev) || (flags & VFS_MOUNT_READ_ONLY) != 0U;
	fat->root[0] = (char)('0' + slot);
	fat->root[1] = ':';
	result = err_of(f_mount(&fat->fs, fat->root, 1));
	if (result != 0) {
		pr_err("mount %s: FatFs %d\n", bdev_name(bdev), (int)result);
		(void)f_mount(NULL, fat->root, 0);
		drive[slot] = NULL;
		free(fat);
		return result;
	}
	*priv = fat;
	return 0;
}

/**
 * @brief Unmount a FAT volume and release its drive.
 *
 * @param[in] private_data Mount state.
 * @return 0, or the mapped FatFs error.
 *
 * The library is unmounted first; the drive is released and the state freed regardless, so a
 * failing medium cannot pin the slot.
 */
static int fat_umount(void *private_data)
{
	struct fat_priv *fat = private_data;
	int result = err_of(f_mount(NULL, fat->root, 0));

	drive[fat->drv] = NULL;
	free(fat);
	return result;
}

/**
 * @brief Flush a FAT mount.
 *
 * @param[in] private_data Mount state; unused.
 * @return Always 0.
 *
 * FatFs writes its cached window when a file is synced or closed, so the mount itself has
 * nothing to flush.
 */
static int fat_sync(void *private_data)
{
	(void)private_data;
	/* the library writes its window when a file is synced or closed */
	return 0;
}

/**
 * @brief Select the FatFs open mode for the VFS flags.
 *
 * @param[in] flags     VFS_O_* flags.
 * @param[in] read_only True when the mount is read-only, which drops every write mode.
 * @return The FA_* mode word passed to f_open().
 *
 * A create request becomes FA_CREATE_NEW with VFS_O_EXCL, FA_CREATE_ALWAYS with VFS_O_TRUNC,
 * and FA_OPEN_ALWAYS otherwise; without it the file must already exist.
 */
static BYTE open_mode(uint32_t flags, bool read_only)
{
	BYTE mode = 0U;

	if ((flags & VFS_O_READ) != 0U) {
		mode |= FA_READ;
	}
	if ((flags & VFS_O_WRITE) != 0U && !read_only) {
		mode |= FA_WRITE;
	}
	if ((flags & (VFS_O_CREATE | VFS_O_TRUNC)) != 0U && !read_only) {
		if ((flags & VFS_O_EXCL) != 0U) {
			mode |= FA_CREATE_NEW;
		} else if ((flags & VFS_O_TRUNC) != 0U) {
			mode |= FA_CREATE_ALWAYS;
		} else {
			mode |= FA_OPEN_ALWAYS;
		}
	} else {
		mode |= FA_OPEN_EXISTING;
	}
	return mode;
}

/**
 * @brief Open a file on a FAT mount.
 *
 * @param[in]  private_data Mount state.
 * @param[in]  path         Path relative to the mount point.
 * @param[in]  flags        VFS_O_* flags.
 * @param[out] file         Receives the allocated file object.
 * @return 0 on success, -EROFS, -ENOMEM, -ENAMETOOLONG, or the mapped FatFs error.
 */
static int fat_open(void *private_data, const char *path, uint32_t flags, void **file)
{
	struct fat_priv *fat = private_data;
	struct fat_file *handle;
	int result;

	if ((flags & VFS_O_WRITE) != 0U && fat->read_only) {
		return -EROFS;
	}
	PATH(fat, path, full);
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		return -ENOMEM;
	}
	result = err_of(f_open(&handle->file, full, open_mode(flags, fat->read_only)));
	if (result != 0) {
		free(handle);
		return result;
	}
	*file = handle;
	return 0;
}

/**
 * @brief Read from a FAT file.
 *
 * @param[in]  file   File object.
 * @param[out] buffer Destination buffer.
 * @param[in]  length Maximum number of bytes to read.
 * @return The number of bytes read, or a negative errno.
 */
static int fat_read(void *file, void *buffer, size_t length)
{
	struct fat_file *handle = file;
	UINT got = 0U;
	FRESULT result = f_read(&handle->file, buffer, (UINT)length, &got);

	return result != FR_OK ? err_of(result) : (int)got;
}

/**
 * @brief Write to a FAT file.
 *
 * @param[in] file   File object.
 * @param[in] buffer Source buffer.
 * @param[in] length Number of bytes to write.
 * @return The number of bytes written, or a negative errno.
 */
static int fat_write(void *file, const void *buffer, size_t length)
{
	struct fat_file *handle = file;
	UINT put = 0U;
	FRESULT result = f_write(&handle->file, buffer, (UINT)length, &put);

	return result != FR_OK ? err_of(result) : (int)put;
}

/**
 * @brief Move the position of a FAT file.
 *
 * @param[in] file   File object.
 * @param[in] offset Relative or absolute offset; a negative value is applied as an FSIZE_t.
 * @param[in] whence 0 from the start, 1 from the current position, 2 from the end.
 * @return The new position, -EINVAL, or -EIO when the library seek fails.
 */
static int64_t fat_seek(void *file, int64_t offset, int whence)
{
	struct fat_file *handle = file;
	FSIZE_t position;

	if (whence == 0) {
		position = (FSIZE_t)offset;
	} else if (whence == 1) {
		position = f_tell(&handle->file) + (FSIZE_t)offset;
	} else if (whence == 2) {
		position = f_size(&handle->file) + (FSIZE_t)offset;
	} else {
		return -EINVAL;
	}
	if (err_of(f_lseek(&handle->file, position)) != 0) {
		return -EIO;
	}
	return (int64_t)f_tell(&handle->file);
}

/**
 * @brief Resize a FAT file.
 *
 * @param[in] file File object.
 * @param[in] size New size in bytes.
 * @return 0, or -EIO when the seek or the truncate fails.
 */
static int fat_truncate(void *file, uint64_t size)
{
	struct fat_file *handle = file;

	if (err_of(f_lseek(&handle->file, (FSIZE_t)size)) != 0) {
		return -EIO;
	}
	return err_of(f_truncate(&handle->file));
}

/**
 * @brief Flush a FAT file.
 *
 * @param[in] file File object.
 * @return 0, or the mapped FatFs error.
 */
static int fat_flush(void *file)
{
	struct fat_file *handle = file;

	return err_of(f_sync(&handle->file));
}

/**
 * @brief Close a FAT file and free its object.
 *
 * @param[in] file File object.
 * @return 0, or the mapped FatFs error from the library close.
 */
static int fat_close(void *file)
{
	struct fat_file *handle = file;
	int result = err_of(f_close(&handle->file));

	free(handle);
	return result;
}

/**
 * @brief Report the size of an open FAT file.
 *
 * @param[in]  file File object.
 * @param[out] st   Structure receiving the size.
 * @return 0.
 */
static int fat_fstat(void *file, struct vfs_stat *st)
{
	struct fat_file *handle = file;

	memset(st, 0, sizeof(*st));
	st->size = f_size(&handle->file);
	return 0;
}

/**
 * @brief Stat a path on a FAT mount.
 *
 * @param[in]  private_data Mount state.
 * @param[in]  path         Path relative to the mount point.
 * @param[out] st           Structure receiving size, type, attributes and timestamp.
 * @return 0, -ENAMETOOLONG, or the mapped FatFs error.
 */
static int fat_stat(void *private_data, const char *path, struct vfs_stat *st)
{
	struct fat_priv *fat = private_data;
	FILINFO info;
	int result;

	PATH(fat, path, full);
	result = err_of(f_stat(full, &info));
	if (result != 0) {
		return result;
	}
	memset(st, 0, sizeof(*st));
	st->size = info.fsize;
	st->dir = (info.fattrib & AM_DIR) != 0U;
	st->read_only = (info.fattrib & AM_RDO) != 0U;
	st->date = info.fdate;
	st->time = info.ftime;
	return 0;
}

/**
 * @brief Create a directory on a FAT mount.
 *
 * @param[in] private_data Mount state.
 * @param[in] path         Path relative to the mount point.
 * @return 0, -EROFS, -ENAMETOOLONG, or the mapped FatFs error.
 */
static int fat_mkdir(void *private_data, const char *path)
{
	struct fat_priv *fat = private_data;

	if (fat->read_only) {
		return -EROFS;
	}
	PATH(fat, path, full);
	return err_of(f_mkdir(full));
}

/**
 * @brief Remove a file on a FAT mount.
 *
 * @param[in] private_data Mount state.
 * @param[in] path         Path relative to the mount point.
 * @return 0, -EROFS, -ENAMETOOLONG, or the mapped FatFs error.
 */
static int fat_unlink(void *private_data, const char *path)
{
	struct fat_priv *fat = private_data;

	if (fat->read_only) {
		return -EROFS;
	}
	PATH(fat, path, full);
	return err_of(f_unlink(full));
}

/**
 * @brief Remove a directory on a FAT mount.
 *
 * @param[in] private_data Mount state.
 * @param[in] path         Path relative to the mount point.
 * @return 0, -EROFS, -ENOTDIR when the path is a file, -ENAMETOOLONG, or the mapped FatFs
 *         error.
 *
 * FatFs has one remove call for both types, so the entry is stat'ed first and a file is
 * refused before anything is deleted.
 */
static int fat_rmdir(void *private_data, const char *path)
{
	struct fat_priv *fat = private_data;
	FILINFO info;

	if (fat->read_only) {
		return -EROFS;
	}
	PATH(fat, path, full);
	if (f_stat(full, &info) == FR_OK && (info.fattrib & AM_DIR) == 0U) {
		return -ENOTDIR;
	}
	return err_of(f_unlink(full));
}

/**
 * @brief Rename an entry on a FAT mount.
 *
 * @param[in] private_data Mount state.
 * @param[in] from         Existing path.
 * @param[in] to           New path.
 * @return 0, -EROFS, -ENAMETOOLONG, or the mapped FatFs error.
 */
static int fat_rename(void *private_data, const char *from, const char *to)
{
	struct fat_priv *fat = private_data;
	char full_from[VFS_PATH_MAX + 4];
	char full_to[VFS_PATH_MAX + 4];
	int result;

	if (fat->read_only) {
		return -EROFS;
	}
	result = full_path(fat, from, full_from, sizeof(full_from));
	if (result == 0) {
		result = full_path(fat, to, full_to, sizeof(full_to));
	}
	if (result != 0) {
		return result;
	}
	return err_of(f_rename(full_from, full_to));
}

/**
 * @brief Open a directory on a FAT mount.
 *
 * @param[in]  private_data Mount state.
 * @param[in]  path         Path relative to the mount point.
 * @param[out] dir          Receives the allocated directory object.
 * @return 0, -ENOMEM, -ENAMETOOLONG, or the mapped FatFs error.
 */
static int fat_opendir(void *private_data, const char *path, void **dir)
{
	struct fat_priv *fat = private_data;
	struct fat_dir *handle;
	int result;

	PATH(fat, path, full);
	handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		return -ENOMEM;
	}
	result = err_of(f_opendir(&handle->dir, full));
	if (result != 0) {
		free(handle);
		return result;
	}
	*dir = handle;
	return 0;
}

/**
 * @brief Read the next entry of a FAT directory.
 *
 * @param[in]  dir   Directory object.
 * @param[out] entry Entry receiving name, size, type, attributes and timestamp.
 * @return 0 on success, -ENOENT at the end of the directory, or a negative errno.
 *
 * FatFs signals the end with an empty name; that becomes -ENOENT so vfs_readdir() has one
 * convention for both file systems.
 */
static int fat_readdir(void *dir, struct vfs_dirent *entry)
{
	struct fat_dir *handle = dir;
	FRESULT result = f_readdir(&handle->dir, &handle->info);

	if (result != FR_OK) {
		return err_of(result);
	}
	if (handle->info.fname[0] == '\0') {
		return -ENOENT;
	}
	memset(entry, 0, sizeof(*entry));
	snprintf(entry->name, sizeof(entry->name), "%s", handle->info.fname);
	entry->st.size = handle->info.fsize;
	entry->st.dir = (handle->info.fattrib & AM_DIR) != 0U;
	entry->st.read_only = (handle->info.fattrib & AM_RDO) != 0U;
	entry->st.date = handle->info.fdate;
	entry->st.time = handle->info.ftime;
	return 0;
}

/**
 * @brief Close a FAT directory and free its object.
 *
 * @param[in] dir Directory object.
 * @return 0, or the mapped FatFs error from the library close.
 */
static int fat_closedir(void *dir)
{
	struct fat_dir *handle = dir;
	int result = err_of(f_closedir(&handle->dir));

	free(handle);
	return result;
}

/**
 * @brief Report the capacity of a FAT mount.
 *
 * @param[in]  private_data Mount state.
 * @param[out] total        Receives the total size in bytes.
 * @param[out] free_bytes   Receives the free space in bytes.
 * @return 0, or -EIO when the free-cluster count cannot be read.
 *
 * The count comes from f_getfree(); the cluster size of the mounted volume turns both cluster
 * counts into bytes.
 */
static int fat_statfs(void *private_data, uint64_t *total, uint64_t *free_bytes)
{
	struct fat_priv *fat = private_data;
	DWORD free_clusters = 0U;
	FATFS *fs = &fat->fs;
	uint64_t cluster_bytes;

	/* the library scans the FAT itself when the cached free count is unknown */
	if (err_of(f_getfree(fat->root, &free_clusters, &fs)) != 0) {
		return -EIO;
	}
	cluster_bytes = (uint64_t)fs->csize * 512U;
	*total = (uint64_t)(fs->n_fatent - 2U) * cluster_bytes;
	*free_bytes = (uint64_t)free_clusters * cluster_bytes;
	return 0;
}

/**
 * @brief Describe a FAT mount: label and exact FAT generation.
 *
 * @param[in]  private_data Mount state.
 * @param[out] info         Volume information; @c label and @c fstype are filled.
 * @return 0.
 *
 * The library reports FS_EXFAT, FS_FAT32, FS_FAT16, or FS_FAT12; anything else is reported as
 * FAT12.
 */
static int fat_volume_info(void *private_data, struct vfs_volume_info *info)
{
	struct fat_priv *fat = private_data;
	char label[24] = { 0 };
	BYTE type = fat->fs.fs_type;

	(void)f_getlabel(fat->root, label, NULL);
	snprintf(info->label, sizeof(info->label), "%s", label);
	if (type == FS_EXFAT) {
		snprintf(info->fstype, sizeof(info->fstype), "exfat");
	} else if (type == FS_FAT32) {
		snprintf(info->fstype, sizeof(info->fstype), "fat32");
	} else if (type == FS_FAT16) {
		snprintf(info->fstype, sizeof(info->fstype), "fat16");
	} else {
		snprintf(info->fstype, sizeof(info->fstype), "fat12");
	}
	return 0;
}

/**
 * @brief Estimate how many sectors a format will write, for the progress bar.
 *
 * @param[in] bdev     Device being formatted.
 * @param[in] au_bytes Allocation unit in bytes, or zero for the library's automatic choice.
 * @param[in] format   FM_* format selection.
 * @return The estimated sector count, or zero when no estimate is possible.
 *
 * The device is a partition, so the volume gets no partition table of its own (FM_SFD). The
 * label is written with a short mount; the drive is taken and released around the whole call.
 *
 * The count follows the same geometry rules f_mkfs() uses: the reserved area, both FATs and the
 * root directory, plus the exFAT allocation bitmap, up-case table and root directory. It only
 * drives the progress bar; a slightly low value is clamped so the bar never passes 100% before
 * the format really finishes.
 */
static uint64_t fat_format_expected(struct block_device *bdev, uint32_t au_bytes, BYTE format)
{
	/** @brief Cluster sizes in 128K sector units, used by the automatic choice. */
	static const uint16_t cst32[] = { 1, 2, 4, 8, 16, 32, 0 }; /* 128K sector units */
	uint64_t sectors = bdev_nr_sectors(bdev);
	uint64_t cluster;
	uint64_t fat_sectors;

	if (sectors == 0U) {
		return 0U;
	}
	cluster = au_bytes != 0U ? au_bytes / 512U : 0U;
	if ((format & FM_EXFAT) != 0) {
		if (cluster == 0U) {
			cluster = sectors >= 0x4000000ULL ? 256U : sectors >= 0x80000ULL ? 64U : 8U;
		}
		if (cluster == 0U) {
			return 0U;
		}
		fat_sectors = ((sectors / cluster + 2U) * 4U + 511U) / 512U;
		/* FAT + allocation bitmap + up-case table + a small root directory */
		{
			uint64_t clusters = sectors / cluster;
			uint64_t bitmap = (clusters + 7U) / 8U;
			uint64_t per_cluster = cluster * 512U;

			return 32U + fat_sectors + (bitmap + per_cluster - 1U) / per_cluster * cluster +
				(5836U + per_cluster - 1U) / per_cluster * cluster + 8U * cluster;
		}
	}
	if (cluster == 0U) {
		uint64_t unit = sectors / 0x20000ULL;
		uint32_t index = 0U;

		cluster = 1U;
		while (cst32[index] != 0U && cst32[index] <= unit) {
			cluster <<= 1;
			index++;
		}
	}
	fat_sectors = (sectors / cluster * 4ULL + 8ULL + 511ULL) / 512ULL;
	return 32U + 2U * fat_sectors + cluster;
}

/**
 * @brief Format a device with one FatFs variant and, optionally, a label.
 *
 * @param[in] bdev  Device to format.
 * @param[in] label Volume label, may be NULL or empty.
 * @param[in] arg   Allocation unit in bytes, or zero for automatic.
 * @param[in] format FM_* format selection; FM_SFD is added here.
 * @return 0, -ENOMEM, -EMFILE when no drive is free, or the mapped FatFs error.
 *
 * f_mkfs() needs a work area, which is allocated from the heap. The drive is taken first and
 * the expected write volume reported to the progress counter. A label is applied by mounting
 * the fresh volume briefly and calling f_setlabel(); the drive is released on every path.
 */
static int fat_format_flags(struct block_device *bdev, const char *label, uint32_t arg, BYTE format)
{
	MKFS_PARM options = {
		.fmt = format | FM_SFD,
		.n_fat = 2,
		.align = 0,
		.n_root = 0,
		.au_size = arg,
	};
	UINT work_size = 32U * 1024U;
	void *work = malloc(work_size);
	char root[3] = "0:";
	int slot;
	int result;

	if (work == NULL) {
		return -ENOMEM;
	}
	slot = drive_take(bdev);
	if (slot < 0) {
		free(work);
		return slot;
	}
	root[0] = (char)('0' + slot);
	vfs_format_expect(bdev, fat_format_expected(bdev, arg, format));
	/* the estimate only feeds the progress bar */
	result = err_of(f_mkfs(root, &options, work, work_size));
	free(work);
	if (result == 0 && label != NULL && label[0] != '\0') {
		FATFS *fs = calloc(1, sizeof(*fs));

		if (fs != NULL) {
			if (f_mount(fs, root, 1) == FR_OK) {
				char name[24];

				snprintf(name, sizeof(name), "%s%s", root, label);
				(void)f_setlabel(name);
				(void)f_mount(NULL, root, 0);
			}
			free(fs);
		}
	}
	drive[slot] = NULL;
	return result;
}

/**
 * @brief Format a device as FAT12/16 or FAT32.
 *
 * @param[in] bdev  Device to format.
 * @param[in] label Volume label, may be NULL.
 * @param[in] arg   Allocation unit in bytes, or zero for automatic.
 * @return 0, or the error from fat_format_flags().
 */
static int fat_format_fat(struct block_device *bdev, const char *label, uint32_t arg)
{
	return fat_format_flags(bdev, label, arg, FM_FAT | FM_FAT32);
}

/**
 * @brief Format a device as FAT32.
 *
 * @param[in] bdev  Device to format.
 * @param[in] label Volume label, may be NULL.
 * @param[in] arg   Allocation unit in bytes, or zero for automatic.
 * @return 0, or the error from fat_format_flags().
 */
static int fat_format_fat32(struct block_device *bdev, const char *label, uint32_t arg)
{
	return fat_format_flags(bdev, label, arg, FM_FAT32);
}

/**
 * @brief Format a device as exFAT.
 *
 * @param[in] bdev  Device to format.
 * @param[in] label Volume label, may be NULL.
 * @param[in] arg   Allocation unit in bytes, or zero for automatic.
 * @return 0, or the error from fat_format_flags().
 */
static int fat_format_exfat(struct block_device *bdev, const char *label, uint32_t arg)
{
	return fat_format_flags(bdev, label, arg, FM_EXFAT);
}

/** @brief The "fat" VFS file system type. */
static const struct vfs_fs fat_type = {
	.name = "fat",
	.flags = 0U,
	.mount = fat_mount,
	.umount = fat_umount,
	.sync = fat_sync,
	.format = fat_format_fat,
	.open = fat_open,
	.read = fat_read,
	.write = fat_write,
	.seek = fat_seek,
	.truncate = fat_truncate,
	.flush = fat_flush,
	.close = fat_close,
	.fstat = fat_fstat,
	.stat = fat_stat,
	.mkdir = fat_mkdir,
	.unlink = fat_unlink,
	.rmdir = fat_rmdir,
	.rename = fat_rename,
	.opendir = fat_opendir,
	.readdir = fat_readdir,
	.closedir = fat_closedir,
	.statfs = fat_statfs,
	.volume_info = fat_volume_info,
};

/* "fat32" and "exfat" pick the same library; it reads the type from the volume. */
/** @brief "fat32": the same library, with the format hook pinned to FAT32. */
static struct vfs_fs fat32_type;
/** @brief "exfat": the same library, with the format hook pinned to exFAT. */
static struct vfs_fs exfat_type;

/**
 * @brief Init call that registers the FAT file system types.
 *
 * @return 0 on success, or the negative errno from vfs_ff_register().
 */
static int fat_init(void)
{
	return vfs_ff_register();
}
/* before the scheduler: only the type table changes */
INITCALL(1, 10, fat_init);

/**
 * @brief Register the FatFs-backed file system types.
 *
 * @return 0 on success, or the negative errno from vfs_register().
 *
 * The base "fat" type is registered first; "fat32" and "exfat" are copies of it that differ
 * only in the name and the format hook, so the library still detects the volume's own type.
 * A failure part-way leaves the types already registered in place.
 */
int vfs_ff_register(void)
{
	int result = vfs_register(&fat_type);

	if (result != 0) {
		return result;
	}
	fat32_type = fat_type;
	fat32_type.name = "fat32";
	fat32_type.format = fat_format_fat32;
	result = vfs_register(&fat32_type);
	if (result != 0) {
		return result;
	}
	exfat_type = fat_type;
	exfat_type.name = "exfat";
	exfat_type.format = fat_format_exfat;
	return vfs_register(&exfat_type);
}
