/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lwext4 as a VFS file system type: ext2, ext3 and ext4.
 *
 * The library reads and writes 512-byte physical blocks through the block layer; a bounce buffer
 * covers the buffers it hands down unaligned (the card driver only does DMA on aligned ones).
 * A damaged superblock is refused here, before lwext4 starts walking block groups.
 */

#define LOG_MODULE "ext"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <init.h>
#include <log.h>
#include <vfs.h>

#include "ext4.h"
#include "ext4_inode.h"
#include "ext4_mkfs.h"
#include "ext4_oflags.h"


#ifndef CONFIG_EXT_FS_MAX_MOUNTS
#define CONFIG_EXT_FS_MAX_MOUNTS 3
#endif

#define EXT_SECTOR_SIZE	 512U
#define EXT_MAGIC_OFFSET 56U
#define EXT_MAGIC	 0xef53U
#define EXT_BOUNCE_SECTORS 16U

struct ext_mount {
	struct block_device *bdev;
	struct ext4_blockdev ext_bdev;
	struct ext4_blockdev_iface iface;
	char device_name[16];
	char mount_name[16];
	uint8_t physical_buffer[EXT_SECTOR_SIZE] __attribute__((aligned(64)));
	uint8_t bounce[EXT_BOUNCE_SECTORS * EXT_SECTOR_SIZE] __attribute__((aligned(64)));
	bool registered;
	bool mounted;
	bool read_only;
};

struct ext_file {
	struct ext_mount *mount;
	ext4_file file;
};

struct ext_dir {
	struct ext_mount *mount;
	ext4_dir dir;
	char path[VFS_PATH_MAX];
};

static struct ext_mount ext_mounts[CONFIG_EXT_FS_MAX_MOUNTS];

static struct ext_mount *mount_from_bdev(struct ext4_blockdev *ext_bdev)
{
	return ext_bdev != NULL && ext_bdev->bdif != NULL ? ext_bdev->bdif->p_user : NULL;
}

static int ext_error(int result)
{
	return result == EOK ? 0 : -result;
}

/* ---- the block device the library sees ---- */

static int ext_device_open(struct ext4_blockdev *ext_bdev)
{
	return mount_from_bdev(ext_bdev) != NULL ? EOK : ENODEV;
}

static int ext_device_read(struct ext4_blockdev *ext_bdev, void *buffer, uint64_t block, uint32_t count)
{
	struct ext_mount *mount = mount_from_bdev(ext_bdev);
	uint8_t *output = buffer;

	if (mount == NULL) {
		return ENODEV;
	}
	if (((uintptr_t)buffer & 63U) == 0U) {
		return bdev_read(mount->bdev, block, buffer, count) == 0 ? EOK : EIO;
	}
	while (count != 0U) {
		uint32_t chunk = count < EXT_BOUNCE_SECTORS ? count : EXT_BOUNCE_SECTORS;

		if (bdev_read(mount->bdev, block, mount->bounce, chunk) != 0) {
			return EIO;
		}
		memcpy(output, mount->bounce, (size_t)chunk * EXT_SECTOR_SIZE);
		output += (size_t)chunk * EXT_SECTOR_SIZE;
		block += chunk;
		count -= chunk;
	}
	return EOK;
}

static int ext_device_write(struct ext4_blockdev *ext_bdev, const void *buffer, uint64_t block, uint32_t count)
{
	struct ext_mount *mount = mount_from_bdev(ext_bdev);
	const uint8_t *input = buffer;

	if (mount == NULL) {
		return ENODEV;
	}
	if (mount->read_only) {
		return EROFS;
	}
	vfs_format_report(mount->bdev, block + count, bdev_nr_sectors(mount->bdev));
	while (count != 0U) {
		uint32_t chunk = count < EXT_BOUNCE_SECTORS ? count : EXT_BOUNCE_SECTORS;

		if (((uintptr_t)input & 63U) == 0U && count >= EXT_BOUNCE_SECTORS) {
			/* a whole aligned run goes out in one call */
			if (bdev_write(mount->bdev, block, input, count) != 0) {
				return EIO;
			}
			return EOK;
		}
		memcpy(mount->bounce, input, (size_t)chunk * EXT_SECTOR_SIZE);
		if (bdev_write(mount->bdev, block, mount->bounce, chunk) != 0) {
			return EIO;
		}
		input += (size_t)chunk * EXT_SECTOR_SIZE;
		block += chunk;
		count -= chunk;
	}
	return EOK;
}

static int ext_device_close(struct ext4_blockdev *ext_bdev)
{
	struct ext_mount *mount = mount_from_bdev(ext_bdev);

	return mount != NULL ? EOK : ENODEV;
}

/* ---- mounting ---- */

static bool ext_has_magic(struct block_device *bdev)
{
	uint8_t sector[EXT_SECTOR_SIZE] __attribute__((aligned(64)));

	if (bdev_nr_sectors(bdev) < 3U || bdev_read(bdev, 2U, sector, 1U) != 0) {
		return false;
	}
	return sector[EXT_MAGIC_OFFSET] == (uint8_t)EXT_MAGIC &&
		sector[EXT_MAGIC_OFFSET + 1U] == (uint8_t)(EXT_MAGIC >> 8);
}

static uint32_t ext_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
		(uint32_t)bytes[3] << 24;
}

/*
 * A superblock that survived the magic check can still be nonsense, and the mount would then walk
 * block groups the volume does not have or replay a journal larger than the card. Refuse it here.
 */
static bool ext_superblock_sane(struct block_device *bdev)
{
	uint8_t sector[EXT_SECTOR_SIZE] __attribute__((aligned(64)));
	uint64_t device_bytes = (uint64_t)bdev_nr_sectors(bdev) * EXT_SECTOR_SIZE;
	uint64_t volume_bytes;
	uint32_t log_block_size;
	uint32_t block_size;
	uint32_t blocks;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;

	if (bdev_nr_sectors(bdev) < 3U || bdev_read(bdev, 2U, sector, 1U) != 0) {
		return false;
	}
	log_block_size = ext_le32(sector + 0x18U);
	inodes_per_group = ext_le32(sector + 0x28U);
	blocks = ext_le32(sector + 0x04U);
	blocks_per_group = ext_le32(sector + 0x20U);
	if (log_block_size > 6U) {
		return false;
	}
	block_size = 1024U << log_block_size;
	if (block_size > 65536U) {
		return false;
	}
	volume_bytes = (uint64_t)blocks * block_size;
	/* The volume cannot be larger than the partition, and its groups must describe it. */
	if (blocks == 0U || volume_bytes > device_bytes) {
		return false;
	}
	if (blocks_per_group < 256U || blocks_per_group > block_size * 8U) {
		return false;
	}
	if (inodes_per_group == 0U || inodes_per_group > block_size * 8U) {
		return false;
	}
	if ((uint64_t)blocks / blocks_per_group > 1U << 20) {
		return false;
	}
	return true;
}

static struct ext_mount *ext_alloc_mount(struct block_device *bdev)
{
	for (unsigned int index = 0U; index < CONFIG_EXT_FS_MAX_MOUNTS; index++) {
		struct ext_mount *mount = &ext_mounts[index];

		if (!mount->mounted) {
			if (mount->bdev == NULL) {
				snprintf(mount->device_name, sizeof(mount->device_name), "extdev%u", index);
				snprintf(mount->mount_name, sizeof(mount->mount_name), "/ext%u/", index);
			}
			mount->bdev = bdev;
			return mount;
		}
	}
	return NULL;
}

static void ext_setup_blockdev(struct ext_mount *mount)
{
	mount->iface.open = ext_device_open;
	mount->iface.bread = ext_device_read;
	mount->iface.bwrite = ext_device_write;
	mount->iface.close = ext_device_close;
	mount->iface.ph_bsize = EXT_SECTOR_SIZE;
	mount->iface.ph_bcnt = bdev_nr_sectors(mount->bdev);
	mount->iface.ph_bbuf = mount->physical_buffer;
	mount->iface.p_user = mount;
	mount->ext_bdev.bdif = &mount->iface;
	mount->ext_bdev.part_offset = 0U;
	mount->ext_bdev.part_size = bdev_nr_sectors(mount->bdev) * EXT_SECTOR_SIZE;
}

static int ext_vfs_mount(struct block_device *bdev, unsigned int flags, void **private_data)
{
	struct ext_mount *mount;
	int result;

	if (!ext_has_magic(bdev) || !ext_superblock_sane(bdev)) {
		return -EINVAL;
	}
	mount = ext_alloc_mount(bdev);
	if (mount == NULL) {
		return -ENOMEM;
	}
	mount->read_only = bdev_read_only(bdev) || (flags & VFS_MOUNT_READ_ONLY) != 0U;
	ext_setup_blockdev(mount);
	if (!mount->registered) {
		result = ext4_device_register(&mount->ext_bdev, mount->device_name);
		if (result != EOK) {
			return ext_error(result);
		}
		mount->registered = true;
	}
	result = ext4_mount(mount->device_name, mount->mount_name, mount->read_only);
	if (result != EOK) {
		return ext_error(result);
	}
	if (!mount->read_only) {
		result = ext4_recover(mount->mount_name);
		if (result == EOK) {
			result = ext4_journal_start(mount->mount_name);
		}
		if (result != EOK) {
			(void)ext4_umount(mount->mount_name);
			return ext_error(result);
		}
	}
	mount->mounted = true;
	*private_data = mount;
	return 0;
}

static int ext_vfs_umount(void *private_data)
{
	struct ext_mount *mount = private_data;
	int result = mount->read_only ? EOK : ext4_journal_stop(mount->mount_name);

	if (result == EOK) {
		result = ext4_umount(mount->mount_name);
	}
	if (result == EOK) {
		mount->mounted = false;
		result = ext4_device_unregister(mount->device_name);
		if (result == EOK) {
			mount->registered = false;
			mount->bdev = NULL;
			mount->read_only = false;
		}
	}
	return ext_error(result);
}

static int ext_vfs_sync(void *private_data)
{
	struct ext_mount *mount = private_data;

	return mount->read_only ? 0 : ext_error(ext4_cache_flush(mount->mount_name));
}

/* ---- paths and files ---- */

static int ext_full_path(const struct ext_mount *mount, const char *path, char *output, size_t size)
{
	int written = snprintf(output, size, "%s%s", mount->mount_name, path[0] == '/' ? path + 1 : path);

	return written < 0 || (size_t)written >= size ? -ENAMETOOLONG : 0;
}

static int ext_vfs_open(void *private_data, const char *path, unsigned int flags, void **file_data)
{
	struct ext_mount *mount = private_data;
	struct ext_file *file;
	char full[VFS_PATH_MAX];
	int open_flags;
	int result;

	if ((flags & VFS_O_WRITE) != 0U && mount->read_only) {
		return -EROFS;
	}
	if ((flags & (VFS_O_READ | VFS_O_WRITE)) == 0U) {
		return -EINVAL;
	}
	result = ext_full_path(mount, path, full, sizeof(full));
	if (result != 0) {
		return result;
	}
	file = calloc(1, sizeof(*file));
	if (file == NULL) {
		return -ENOMEM;
	}
	open_flags = (flags & VFS_O_READ) != 0U && (flags & VFS_O_WRITE) != 0U ? O_RDWR :
		(flags & VFS_O_WRITE) != 0U ? O_WRONLY : O_RDONLY;
	if ((flags & VFS_O_CREATE) != 0U) {
		open_flags |= O_CREAT;
	}
	if ((flags & VFS_O_EXCL) != 0U) {
		open_flags |= O_EXCL;
	}
	if ((flags & VFS_O_TRUNC) != 0U) {
		open_flags |= O_TRUNC;
	}
	if ((flags & VFS_O_APPEND) != 0U) {
		open_flags |= O_APPEND;
	}
	result = ext_error(ext4_fopen2(&file->file, full, open_flags));
	if (result != 0) {
		free(file);
		return result;
	}
	file->mount = mount;
	*file_data = file;
	return 0;
}

static int ext_vfs_read(void *file_data, void *buffer, size_t length)
{
	struct ext_file *file = file_data;
	size_t got = 0U;
	int result = ext4_fread(&file->file, buffer, length, &got);

	return result == EOK ? (int)got : ext_error(result);
}

static int ext_vfs_write(void *file_data, const void *buffer, size_t length)
{
	struct ext_file *file = file_data;
	size_t put = 0U;
	int result;

	if (file->mount->read_only) {
		return -EROFS;
	}
	result = ext4_fwrite(&file->file, buffer, length, &put);
	return result == EOK ? (int)put : ext_error(result);
}

static int64_t ext_vfs_seek(void *file_data, int64_t offset, int whence)
{
	struct ext_file *file = file_data;
	uint64_t base;
	uint64_t target;

	if (whence == 0) {
		base = 0U;
	} else if (whence == 1) {
		base = ext4_ftell(&file->file);
	} else if (whence == 2) {
		base = ext4_fsize(&file->file);
	} else {
		return -EINVAL;
	}
	if (offset < 0) {
		uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;

		if (magnitude > base) {
			return -EINVAL;
		}
		target = base - magnitude;
	} else {
		if ((uint64_t)offset > UINT64_MAX - base) {
			return -EINVAL;
		}
		target = base + (uint64_t)offset;
	}
	if (ext4_fseek(&file->file, target, 0U) != EOK) {
		return -EINVAL;
	}
	return (int64_t)ext4_ftell(&file->file);
}

static int ext_vfs_truncate(void *file_data, uint64_t size)
{
	struct ext_file *file = file_data;

	return file->mount->read_only ? -EROFS : ext_error(ext4_ftruncate(&file->file, size));
}

static int ext_vfs_flush(void *file_data)
{
	struct ext_file *file = file_data;

	return file->mount->read_only ? 0 : ext_error(ext4_cache_flush(file->mount->mount_name));
}

static int ext_vfs_close(void *file_data)
{
	struct ext_file *file = file_data;
	int result = ext_error(ext4_fclose(&file->file));

	free(file);
	return result;
}

static int ext_vfs_fstat(void *file_data, struct vfs_stat *st)
{
	struct ext_file *file = file_data;

	memset(st, 0, sizeof(*st));
	st->size = ext4_fsize(&file->file);
	st->read_only = file->mount->read_only;
	return 0;
}

static int ext_stat_path(struct ext_mount *mount, const char *path, struct vfs_stat *st)
{
	struct ext4_sblock *superblock;
	struct ext4_inode inode;
	char full[VFS_PATH_MAX];
	uint32_t inode_number;
	uint32_t mode;
	int result = ext_full_path(mount, path, full, sizeof(full));

	if (result != 0) {
		return result;
	}
	result = ext4_raw_inode_fill(full, &inode_number, &inode);
	if (result != EOK) {
		return ext_error(result);
	}
	result = ext4_get_sblock(mount->mount_name, &superblock);
	if (result != EOK) {
		return ext_error(result);
	}
	mode = ext4_inode_get_mode(superblock, &inode) & EXT4_INODE_MODE_TYPE_MASK;
	if (mode != EXT4_INODE_MODE_DIRECTORY && mode != EXT4_INODE_MODE_FILE &&
	    mode != EXT4_INODE_MODE_SOFTLINK) {
		return -ENOTSUP;
	}
	memset(st, 0, sizeof(*st));
	st->dir = mode == EXT4_INODE_MODE_DIRECTORY;
	st->size = st->dir ? 0U : ext4_inode_get_size(superblock, &inode);
	st->read_only = mount->read_only;
	return 0;
}

static int ext_vfs_stat(void *private_data, const char *path, struct vfs_stat *st)
{
	return ext_stat_path(private_data, path, st);
}

static int ext_vfs_mkdir(void *private_data, const char *path)
{
	struct ext_mount *mount = private_data;
	char full[VFS_PATH_MAX];
	int result;

	if (mount->read_only) {
		return -EROFS;
	}
	result = ext_full_path(mount, path, full, sizeof(full));
	return result == 0 ? ext_error(ext4_dir_mk(full)) : result;
}

static int ext_vfs_unlink(void *private_data, const char *path)
{
	struct ext_mount *mount = private_data;
	char full[VFS_PATH_MAX];
	int result;

	if (mount->read_only) {
		return -EROFS;
	}
	result = ext_full_path(mount, path, full, sizeof(full));
	return result == 0 ? ext_error(ext4_fremove(full)) : result;
}

static int ext_vfs_rmdir(void *private_data, const char *path)
{
	struct ext_mount *mount = private_data;
	char full[VFS_PATH_MAX];
	int result;

	if (mount->read_only) {
		return -EROFS;
	}
	result = ext_full_path(mount, path, full, sizeof(full));
	return result == 0 ? ext_error(ext4_dir_rm(full)) : result;
}

static int ext_vfs_rename(void *private_data, const char *from, const char *to)
{
	struct ext_mount *mount = private_data;
	char old_path[VFS_PATH_MAX];
	char new_path[VFS_PATH_MAX];
	int result;

	if (mount->read_only) {
		return -EROFS;
	}
	result = ext_full_path(mount, from, old_path, sizeof(old_path));
	if (result == 0) {
		result = ext_full_path(mount, to, new_path, sizeof(new_path));
	}
	return result == 0 ? ext_error(ext4_frename(old_path, new_path)) : result;
}

static int ext_vfs_opendir(void *private_data, const char *path, void **dir_data)
{
	struct ext_mount *mount = private_data;
	struct ext_dir *dir;
	char full[VFS_PATH_MAX];
	int result = ext_full_path(mount, path, full, sizeof(full));

	if (result != 0) {
		return result;
	}
	dir = calloc(1, sizeof(*dir));
	if (dir == NULL) {
		return -ENOMEM;
	}
	result = ext_error(ext4_dir_open(&dir->dir, full));
	if (result != 0) {
		free(dir);
		return result;
	}
	dir->mount = mount;
	snprintf(dir->path, sizeof(dir->path), "%s", path);
	*dir_data = dir;
	return 0;
}

static int ext_vfs_readdir(void *dir_data, struct vfs_dirent *entry)
{
	struct ext_dir *dir = dir_data;
	const ext4_direntry *ext_entry;
	char path[VFS_PATH_MAX];
	int length;

	for (;;) {
		do {
			ext_entry = ext4_dir_entry_next(&dir->dir);
			if (ext_entry == NULL) {
				return -ENOENT;
			}
		} while ((ext_entry->name_length == 1U && ext_entry->name[0] == '.') ||
			 (ext_entry->name_length == 2U && ext_entry->name[0] == '.' && ext_entry->name[1] == '.'));
		memcpy(entry->name, ext_entry->name, ext_entry->name_length);
		entry->name[ext_entry->name_length] = '\0';
		if (strcmp(dir->path, "/") == 0) {
			length = snprintf(path, sizeof(path), "/%s", entry->name);
		} else {
			length = snprintf(path, sizeof(path), "%s/%s", dir->path, entry->name);
		}
		if (length < 0 || (size_t)length >= sizeof(path)) {
			return -ENAMETOOLONG;
		}
		length = ext_stat_path(dir->mount, path, &entry->st);
		if (length != -ENOTSUP) {
			return length;
		}
	}
}

static int ext_vfs_closedir(void *dir_data)
{
	struct ext_dir *dir = dir_data;
	int result = ext_error(ext4_dir_close(&dir->dir));

	free(dir);
	return result;
}

static int ext_vfs_statfs(void *private_data, uint64_t *total, uint64_t *free_bytes)
{
	struct ext_mount *mount = private_data;
	struct ext4_mount_stats stats;
	int result = ext4_mount_point_stats(mount->mount_name, &stats);

	if (result != EOK) {
		return ext_error(result);
	}
	*total = stats.blocks_count * stats.block_size;
	*free_bytes = stats.free_blocks_count * stats.block_size;
	if (*free_bytes > *total) {
		*free_bytes = *total;
	}
	return 0;
}

static int ext_vfs_volume_info(void *private_data, struct vfs_volume_info *info)
{
	struct ext_mount *mount = private_data;
	struct ext4_mount_stats stats;
	struct ext4_sblock *superblock;
	int result = ext4_mount_point_stats(mount->mount_name, &stats);

	if (result == EOK) {
		result = ext4_get_sblock(mount->mount_name, &superblock);
	}
	if (result != EOK) {
		return ext_error(result);
	}
	snprintf(info->label, sizeof(info->label), "%s", stats.volume_name);
	snprintf(info->fstype, sizeof(info->fstype), "%s",
		(ext4_get32(superblock, features_incompatible) & EXT4_FINCOM_EXTENTS) != 0U ? "ext4" :
		(ext4_get32(superblock, features_compatible) & EXT4_FCOM_HAS_JOURNAL) != 0U ? "ext3" : "ext2");
	return 0;
}

/* A mount context is nearly nine kilobytes; the formatter gets one of its own, not the stack. */
static struct ext_mount ext_format_mount;

static int ext_vfs_format(struct block_device *bdev, const char *label, unsigned int argument)
{
	struct ext_mount *mount = &ext_format_mount;
	struct ext4_mkfs_info info = { .label = label, .journal = argument != F_SET_EXT2 };
	struct ext4_fs file_system = { 0 };
	int result;

	/* The VFS runs one format at a time, so the one context can be reused. */
	memset(mount, 0, sizeof(*mount));
	mount->bdev = bdev;
	mount->read_only = bdev_read_only(bdev);
	if (mount->read_only) {
		return -EROFS;
	}
	ext_setup_blockdev(mount);
	result = ext4_mkfs(&file_system, &mount->ext_bdev, &info,
		argument == F_SET_EXT2 || argument == F_SET_EXT3 ? (int)argument : F_SET_EXT4);
	return result == EOK ? bdev_flush(bdev) : ext_error(result);
}

static const struct vfs_fs ext_type = {
	.name = "ext",
	.flags = 0U,
	.mount = ext_vfs_mount,
	.umount = ext_vfs_umount,
	.sync = ext_vfs_sync,
	.format = ext_vfs_format,
	.open = ext_vfs_open,
	.read = ext_vfs_read,
	.write = ext_vfs_write,
	.seek = ext_vfs_seek,
	.truncate = ext_vfs_truncate,
	.flush = ext_vfs_flush,
	.close = ext_vfs_close,
	.fstat = ext_vfs_fstat,
	.stat = ext_vfs_stat,
	.mkdir = ext_vfs_mkdir,
	.unlink = ext_vfs_unlink,
	.rmdir = ext_vfs_rmdir,
	.rename = ext_vfs_rename,
	.opendir = ext_vfs_opendir,
	.readdir = ext_vfs_readdir,
	.closedir = ext_vfs_closedir,
	.statfs = ext_vfs_statfs,
	.volume_info = ext_vfs_volume_info,
};

int vfs_ext_register(void)
{
	return vfs_register(&ext_type);
}

static int ext_init(void)
{
	return vfs_ext_register();
}
INITCALL(1, 20, ext_init);
