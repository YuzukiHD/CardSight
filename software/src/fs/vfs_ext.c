/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file vfs_ext.c
 * @brief lwext4 as a VFS file system type: ext2, ext3 and ext4.
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


/** @brief Fallback number of simultaneous ext mounts when the configuration sets none. */
#ifndef CONFIG_EXT_FS_MAX_MOUNTS
#define CONFIG_EXT_FS_MAX_MOUNTS 3
#endif

/** @brief Physical block size seen by lwext4; the ext superblock always sits at 1024 bytes. */
#define EXT_SECTOR_SIZE	 512U
/** @brief Offset of the ext superblock magic inside sector 2 (0x38). */
#define EXT_MAGIC_OFFSET 56U
/** @brief Superblock magic of every ext2/ext3/ext4 volume. */
#define EXT_MAGIC	 0xef53U
/** @brief Sectors moved per bounce-buffer round trip. */
#define EXT_BOUNCE_SECTORS 16U

/**
 * @brief Everything one ext mount owns.
 *
 * A slot is reused across mounts: the names and the library block device survive, only
 * @c bdev and the flags change. The buffers are cache-line aligned because the block layer
 * only does DMA on aligned addresses.
 */
struct ext_mount {
	struct block_device *bdev;	/**< Device holding the volume. */
	struct ext4_blockdev ext_bdev;	/**< Library view of the device. */
	struct ext4_blockdev_iface iface;	/**< Library callbacks and physical geometry. */
	char device_name[16];		/**< Library device name, "extdev<slot>". */
	char mount_name[16];		/**< Library mount point, "/ext<slot>/". */
	uint8_t physical_buffer[EXT_SECTOR_SIZE] __attribute__((aligned(64)));
					/**< Sector buffer the library requires. */
	uint8_t bounce[EXT_BOUNCE_SECTORS * EXT_SECTOR_SIZE] __attribute__((aligned(64)));
					/**< Bounce buffer for unaligned transfers. */
	bool registered;		/**< Block device registered with the library. */
	bool mounted;			/**< Slot currently mounted. */
	bool read_only;			/**< Volume was mounted or formatted read-only. */
};

/**
 * @brief An open file: the mount plus the library file object.
 */
struct ext_file {
	struct ext_mount *mount;	/**< Mount the file lives on. */
	ext4_file file;		/**< Library file object. */
};

/**
 * @brief An open directory: the mount and the library directory object.
 *
 * Library entries carry their inode number, so readdir stats each one directly
 * instead of resolving the name back through the full path.
 */
struct ext_dir {
	struct ext_mount *mount;	/**< Mount the directory lives on. */
	ext4_dir dir;		/**< Library directory object. */
};

/** @brief Mount slots; @c mounted marks the live ones. */
static struct ext_mount ext_mounts[CONFIG_EXT_FS_MAX_MOUNTS];

/**
 * @brief Recover the mount context from a library block device.
 *
 * @param[in] ext_bdev Block device handed to the library.
 * @return The owning mount, or NULL when the device or its interface is not set up.
 *
 * The interface stores the mount in its @c p_user field.
 */
static struct ext_mount *mount_from_bdev(struct ext4_blockdev *ext_bdev)
{
	return ext_bdev != NULL && ext_bdev->bdif != NULL ? ext_bdev->bdif->p_user : NULL;
}

/**
 * @brief Convert an lwext4 status into a negated errno.
 *
 * @param[in] result EOK or a positive lwext4 error code.
 * @return 0 for EOK, otherwise the negated code.
 */
static int ext_error(int result)
{
	return result == EOK ? 0 : -result;
}

/* ---- the block device the library sees ---- */

/**
 * @brief Open hook of the library block device.
 *
 * @param[in] ext_bdev Block device handed to the library.
 * @return EOK when the device is set up, ENODEV otherwise.
 */
static int ext_device_open(struct ext4_blockdev *ext_bdev)
{
	return mount_from_bdev(ext_bdev) != NULL ? EOK : ENODEV;
}

/**
 * @brief Read blocks for lwext4.
 *
 * @param[in]  ext_bdev Block device handed to the library.
 * @param[out] buffer   Destination buffer.
 * @param[in]  block    First physical block (512 bytes) to read.
 * @param[in]  count    Number of blocks.
 * @return EOK, ENODEV, or EIO on a medium error.
 *
 * The card driver only takes cache-line aligned DMA buffers. An aligned request is handed
 * straight to the block layer; anything else is copied through the mount's bounce buffer in
 * chunks of EXT_BOUNCE_SECTORS.
 */
static int ext_device_read(struct ext4_blockdev *ext_bdev, void *buffer, uint64_t block, uint32_t count)
{
	struct ext_mount *mount = mount_from_bdev(ext_bdev);
	uint8_t *output = buffer;

	if (mount == NULL) {
		return ENODEV;
	}
	/* an aligned buffer is what the card driver's DMA wants */
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

/**
 * @brief Write blocks for lwext4.
 *
 * @param[in] ext_bdev Block device handed to the library.
 * @param[in] buffer   Source buffer.
 * @param[in] block    First physical block (512 bytes) to write.
 * @param[in] count    Number of blocks.
 * @return EOK, ENODEV, EROFS, or EIO on a medium error.
 *
 * Reports the write to the VFS format progress counter first. An aligned source covering a
 * whole bounce chunk goes out in one call; any other buffer is copied through the bounce
 * buffer. A read-only mount refuses the write before touching the medium.
 */
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
	vfs_format_report_write(mount->bdev, count);
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

/**
 * @brief Close hook of the library block device.
 *
 * @param[in] ext_bdev Block device handed to the library.
 * @return EOK when the device is set up, ENODEV otherwise.
 */
static int ext_device_close(struct ext4_blockdev *ext_bdev)
{
	struct ext_mount *mount = mount_from_bdev(ext_bdev);

	return mount != NULL ? EOK : ENODEV;
}

/* ---- mounting ---- */

/**
 * @brief Check whether sector 2 carries the ext superblock magic.
 *
 * @param[in] bdev Device to inspect.
 * @return True when the magic is present.
 */
static bool ext_has_magic(struct block_device *bdev)
{
	uint8_t sector[EXT_SECTOR_SIZE] __attribute__((aligned(64)));

	if (bdev_nr_sectors(bdev) < 3U || bdev_read(bdev, 2U, sector, 1U) != 0) {
		return false;
	}
	return sector[EXT_MAGIC_OFFSET] == (uint8_t)EXT_MAGIC &&
		sector[EXT_MAGIC_OFFSET + 1U] == (uint8_t)(EXT_MAGIC >> 8);
}

/**
 * @brief Decode a little-endian 32-bit field.
 *
 * @param[in] bytes Four bytes in on-disk order.
 * @return The decoded value.
 */
static uint32_t ext_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
		(uint32_t)bytes[3] << 24;
}

/**
 * @brief Reject a superblock whose geometry cannot describe this device.
 *
 * @param[in] bdev Device to inspect.
 * @return True when the superblock fields are plausible.
 *
 * A superblock that survived the magic check can still be nonsense, and the mount would then
 * walk block groups the volume does not have or replay a journal larger than the card. The
 * checks are: block size in range, a non-zero block count whose size fits the partition, a
 * plausible block and inode count per group, and a group count that cannot overflow the
 * library's group table. Refuse it here.
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

/**
 * @brief Reserve a mount slot for a device.
 *
 * @param[in] bdev Device to mount.
 * @return A free slot, or NULL when all slots are mounted.
 *
 * The device and mount names are formatted once per slot and then reused.
 */
static struct ext_mount *ext_alloc_mount(struct block_device *bdev)
{
	for (uint32_t index = 0U; index < CONFIG_EXT_FS_MAX_MOUNTS; index++) {
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

/**
 * @brief Fill the library block device for a mount.
 *
 * @param[in,out] mount Mount whose interface and block device are configured.
 *
 * The interface points at the mount through @c p_user, uses 512-byte physical blocks and the
 * mount's aligned physical buffer, and covers the whole opened device.
 */
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

/**
 * @brief Mount an ext volume through lwext4.
 *
 * @param[in]  bdev         Device holding the volume.
 * @param[in]  flags        VFS_MOUNT_* flags; VFS_MOUNT_READ_ONLY forces a read-only mount.
 * @param[out] private_data Receives the mount context.
 * @return 0 on success, -EINVAL for a bad superblock, -ENOMEM when no slot is free, or the
 *         negated lwext4 error.
 *
 * The magic and the superblock geometry are checked before the library is involved. A
 * read-write mount then replays the journal and starts the journaling layer; a failure there
 * unmounts again. Recovering the journal is what makes a card pulled from another system
 * safe to write.
 */
static int ext_vfs_mount(struct block_device *bdev, uint32_t flags, void **private_data)
{
	struct ext_mount *mount;
	int result;

	/* refuse a damaged superblock before lwext4 walks its block groups */
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
	/* a read-write mount replays the journal before it is used */
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

/**
 * @brief Unmount an ext volume.
 *
 * @param[in] private_data Mount context from ext_vfs_mount().
 * @return 0 on success, or the negated lwext4 error.
 *
 * Stops the journal on a read-write mount, unmounts the volume, then unregisters the block
 * device so the slot can be reused. The slot is only cleared once every step succeeded.
 */
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

/**
 * @brief Flush an ext mount's cached writes.
 *
 * @param[in] private_data Mount context.
 * @return 0, or the negated lwext4 error; a read-only mount has nothing to flush.
 */
static int ext_vfs_sync(void *private_data)
{
	struct ext_mount *mount = private_data;

	return mount->read_only ? 0 : ext_error(ext4_cache_flush(mount->mount_name));
}

/* ---- paths and files ---- */

/**
 * @brief Build the library path of a file inside a mount.
 *
 * @param[in]  mount  Mount context.
 * @param[in]  path   Path relative to the mount point.
 * @param[out] output Buffer receiving the mount name plus the path.
 * @param[in]  size   Size of @p output in bytes.
 * @return 0 on success, -ENAMETOOLONG when the result is truncated.
 */
static int ext_full_path(const struct ext_mount *mount, const char *path, char *output, size_t size)
{
	int written = snprintf(output, size, "%s%s", mount->mount_name, path[0] == '/' ? path + 1 : path);

	return written < 0 || (size_t)written >= size ? -ENAMETOOLONG : 0;
}

/**
 * @brief Open a file inside an ext mount.
 *
 * @param[in]  private_data Mount context.
 * @param[in]  path         Path relative to the mount point.
 * @param[in]  flags        VFS_O_* flags.
 * @param[out] file_data    Receives the allocated file object.
 * @return 0 on success, -EROFS, -EINVAL, -ENOMEM, -ENAMETOOLONG, or the negated lwext4 error.
 *
 * A write open on a read-only mount is refused first. The VFS flags are translated into the
 * library's open flags, the file object is allocated, and the library opens the full path.
 */
static int ext_vfs_open(void *private_data, const char *path, uint32_t flags, void **file_data)
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
	/* map the VFS flags onto the lwext4 open flags */
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

/**
 * @brief Read from an ext file.
 *
 * @param[in]  file_data File object.
 * @param[out] buffer    Destination buffer.
 * @param[in]  length    Maximum number of bytes to read.
 * @return The number of bytes read, or the negated lwext4 error.
 */
static int ext_vfs_read(void *file_data, void *buffer, size_t length)
{
	struct ext_file *file = file_data;
	size_t got = 0U;
	int result = ext4_fread(&file->file, buffer, length, &got);

	return result == EOK ? (int)got : ext_error(result);
}

/**
 * @brief Write to an ext file.
 *
 * @param[in] file_data File object.
 * @param[in] buffer    Source buffer.
 * @param[in] length    Number of bytes to write.
 * @return The number of bytes written, -EROFS, or the negated lwext4 error.
 */
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

/**
 * @brief Move the position of an ext file, handling both offset signs.
 *
 * @param[in] file_data File object.
 * @param[in] offset    Relative or absolute offset.
 * @param[in] whence    0 from the start, 1 from the current position, 2 from the end.
 * @return The new position, -EINVAL, or the negated lwext4 error.
 *
 * The signed VFS offset is resolved against the chosen base with explicit bounds checks, then
 * the absolute target is applied to the library, which only seeks from the start.
 */
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

/**
 * @brief Resize an ext file.
 *
 * @param[in] file_data File object.
 * @param[in] size      New size in bytes.
 * @return 0, -EROFS, or the negated lwext4 error.
 */
static int ext_vfs_truncate(void *file_data, uint64_t size)
{
	struct ext_file *file = file_data;

	return file->mount->read_only ? -EROFS : ext_error(ext4_ftruncate(&file->file, size));
}

/**
 * @brief Flush the cached writes of an ext file.
 *
 * @param[in] file_data File object.
 * @return 0, or the negated lwext4 error; a read-only mount has nothing to flush.
 */
static int ext_vfs_flush(void *file_data)
{
	struct ext_file *file = file_data;

	return file->mount->read_only ? 0 : ext_error(ext4_cache_flush(file->mount->mount_name));
}

/**
 * @brief Close an ext file and free its object.
 *
 * @param[in] file_data File object.
 * @return 0, or the negated lwext4 error from the library close.
 */
static int ext_vfs_close(void *file_data)
{
	struct ext_file *file = file_data;
	int result = ext_error(ext4_fclose(&file->file));

	free(file);
	return result;
}

/**
 * @brief Report the size of an open ext file.
 *
 * @param[in]  file_data File object.
 * @param[out] st        Structure receiving size and the read-only flag.
 * @return 0.
 */
static int ext_vfs_fstat(void *file_data, struct vfs_stat *st)
{
	struct ext_file *file = file_data;

	memset(st, 0, sizeof(*st));
	st->size = ext4_fsize(&file->file);
	st->read_only = file->mount->read_only;
	return 0;
}

/**
 * @brief Stat a path inside an ext mount.
 *
 * @param[in]  mount Mount context.
 * @param[in]  path  Path relative to the mount point.
 * @param[out] st    Structure receiving the metadata.
 * @return 0, -ENAMETOOLONG, -ENOTSUP for a special file, or the negated lwext4 error.
 *
 * The raw inode is read to learn the mode: directories, regular files and symbolic links are
 * accepted, everything else is refused. Directories report size 0, as the VFS expects.
 */
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
	/* the inode mode says whether this is a file, a directory, or a link */
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
	/* A directory's i_size is the byte length of its directory data. */
	st->alloc_size = st->dir ? ext4_inode_get_size(superblock, &inode) : 0U;
	st->read_only = mount->read_only;
	return 0;
}

/**
 * @brief Stat one inode by number.
 *
 * @details A directory listing already knows each entry's inode number, so
 * reading the inode directly avoids resolving the full path for every entry.
 * On a directory with more than a thousand files that difference is most of
 * the listing time.
 *
 * @param[in]  mount        Mount the inode belongs to.
 * @param[in]  inode_number Inode number from the directory entry.
 * @param[out] st           Structure receiving the metadata.
 * @return 0, -ENOTSUP for an object the VFS cannot represent, or a negative errno.
 */
static int ext_stat_inode(struct ext_mount *mount, uint32_t inode_number,
	struct vfs_stat *st)
{
	struct ext4_sblock *superblock;
	struct ext4_inode inode;
	uint32_t mode;
	int result = ext4_raw_inode_fill_index(mount->mount_name, inode_number, NULL, &inode);

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
	/* A directory's i_size is the byte length of its directory data. */
	st->alloc_size = st->dir ? ext4_inode_get_size(superblock, &inode) : 0U;
	st->read_only = mount->read_only;
	return 0;
}

/**
 * @brief Stat a path for the VFS.
 *
 * @param[in]  private_data Mount context.
 * @param[in]  path         Path relative to the mount point.
 * @param[out] st           Structure receiving the metadata.
 * @return 0, or the negative errno from ext_stat_path().
 */
static int ext_vfs_stat(void *private_data, const char *path, struct vfs_stat *st)
{
	return ext_stat_path(private_data, path, st);
}

/**
 * @brief Create a directory inside an ext mount.
 *
 * @param[in] private_data Mount context.
 * @param[in] path         Path relative to the mount point.
 * @return 0, -EROFS, -ENAMETOOLONG, or the negated lwext4 error.
 */
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

/**
 * @brief Remove a file inside an ext mount.
 *
 * @param[in] private_data Mount context.
 * @param[in] path         Path relative to the mount point.
 * @return 0, -EROFS, -ENAMETOOLONG, or the negated lwext4 error.
 */
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

/**
 * @brief Remove a directory inside an ext mount.
 *
 * @param[in] private_data Mount context.
 * @param[in] path         Path relative to the mount point.
 * @return 0, -EROFS, -ENAMETOOLONG, or the negated lwext4 error.
 */
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

/**
 * @brief Rename an entry inside an ext mount.
 *
 * @param[in] private_data Mount context.
 * @param[in] from         Existing path.
 * @param[in] to           New path.
 * @return 0, -EROFS, -ENAMETOOLONG, or the negated lwext4 error.
 */
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

/**
 * @brief Open a directory inside an ext mount.
 *
 * @param[in]  private_data Mount context.
 * @param[in]  path         Path relative to the mount point.
 * @param[out] dir_data     Receives the allocated directory object.
 * @return 0, -ENOMEM, -ENAMETOOLONG, or the negated lwext4 error.
 *
 * The VFS path is stored in the object so readdir() can stat each entry by its full path.
 */
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
	*dir_data = dir;
	return 0;
}

/**
 * @brief Read the next entry of an ext directory.
 *
 * @param[in]  dir_data Directory object.
 * @param[out] entry    Entry filled with name and metadata.
 * @return 0 on success, -ENOENT at the end, -ENAMETOOLONG, or a negative errno.
 *
 * "." and ".." are skipped. Each remaining entry is stat'ed through its full path; an entry
 * whose type VFS cannot represent (-ENOTSUP) is skipped rather than failing the listing.
 */
static int ext_vfs_readdir(void *dir_data, struct vfs_dirent *entry)
{
	struct ext_dir *dir = dir_data;
	const ext4_direntry *ext_entry;
	int result;

	/* skip "." and ".." and any entry whose type the VFS cannot represent */
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
		result = ext_stat_inode(dir->mount, ext_entry->inode, &entry->st);
		if (result != -ENOTSUP) {
			return result;
		}
	}
}

/**
 * @brief Close an ext directory and free its object.
 *
 * @param[in] dir_data Directory object.
 * @return 0, or the negated lwext4 error from the library close.
 */
static int ext_vfs_closedir(void *dir_data)
{
	struct ext_dir *dir = dir_data;
	int result = ext_error(ext4_dir_close(&dir->dir));

	free(dir);
	return result;
}

/**
 * @brief Report the capacity of an ext mount.
 *
 * @param[in]  private_data Mount context.
 * @param[out] total        Receives the total size in bytes.
 * @param[out] free_bytes   Receives the free space in bytes.
 * @return 0, or the negated lwext4 error.
 *
 * The free count is clamped to the total, in case the library reports inconsistent counters.
 */
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

/**
 * @brief Describe an ext mount: label and exact file system generation.
 *
 * @param[in]  private_data Mount context.
 * @param[out] info         Volume information; @c label and @c fstype are filled.
 * @return 0, or the negated lwext4 error.
 *
 * The generation comes from the superblock feature flags: extents mean ext4, a journal means
 * ext3, otherwise ext2.
 */
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
	info->used_inodes = stats.inodes_count > stats.free_inodes_count ?
		(uint64_t)(stats.inodes_count - stats.free_inodes_count) : 0U;
	info->cluster_bytes = stats.block_size;
	return 0;
}

/**
 * @brief Format context of its own: a mount context is nearly nine kilobytes, too much for the
 * stack.
 */
static struct ext_mount ext_format_mount;

/**
 * @brief Create an ext file system on a device.
 *
 * @param[in] bdev     Device to format.
 * @param[in] label    Volume label, may be NULL.
 * @param[in] argument F_SET_EXT2, F_SET_EXT3 or F_SET_EXT4 selection.
 * @return 0 on success, -EROFS, or the negated lwext4 error.
 *
 * The formatter reuses the dedicated format context and a scratch superblock, so no mount slot
 * is consumed. Read-only devices are refused, and the device is flushed once the library is
 * done.
 */
static int ext_vfs_format(struct block_device *bdev, const char *label, uint32_t argument)
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

/** @brief The VFS file system type exported by this file. */
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

/**
 * @brief Register the ext file system type with the VFS.
 *
 * @return 0 on success, or the negative errno from vfs_register().
 */
int vfs_ext_register(void)
{
	return vfs_register(&ext_type);
}

/**
 * @brief Init call that registers the ext file system.
 *
 * @return 0 on success, or the negative errno from vfs_register().
 */
static int ext_init(void)
{
	return vfs_ext_register();
}
INITCALL(1, 20, ext_init);
