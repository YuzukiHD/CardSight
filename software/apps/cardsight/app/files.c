// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file files.c
 * @brief Directory listing for the file browser.
 *
 * The proxy may already have the partition mounted; otherwise it is mounted
 * read-only for the length of the request, so browsing never changes the card
 * and never keeps a volume busy that a format or the proxy would need. The
 * listing is one level of the view, and its path is relative to the partition
 * root.
 */

#include "app/app_internal.h"

#include <bits.h>
#include "FreeRTOS.h"
#include <strings.h>
#include <vfs.h>

/**
 * @brief Find the mount target currently backing a block device.
 *
 * @param[in]  block  Block device name to look for.
 * @param[out] target Buffer receiving the mount target.
 * @param[in]  size   Size of @p target in bytes.
 * @return 0 when a mount was found, or -ENOENT.
 */
static int mounted_target(const char *block, char *target, size_t size)
{
	for (uint32_t index = 0U;; index++) {
		struct vfs_mount_info info;
		int result = vfs_mount_get(index, &info);

		if (result != 0) {
			return -ENOENT;
		}
		if (strcmp(info.blk, block) == 0) {
			snprintf(target, size, "%s", info.target);
			return 0;
		}
	}
}

/**
 * @brief Check that a browser path is absolute and cannot climb out of the partition.
 *
 * @param[in] path Path to check.
 * @return True when the path starts at the root and contains no "..".
 */
static bool path_safe(const char *path)
{
	return path[0] == '/' && strstr(path, "..") == NULL;
}

/**
 * @brief Insert one directory entry into the sorted listing.
 *
 * @details Directories first, then names in case-insensitive order, like a
 * file manager. The array is kept sorted by shifting the tail up one slot, so
 * the newest entry lands in the right place without a separate sort pass.
 *
 * @param[in,out] list Listing to insert into.
 * @param[in]     item Directory entry to copy in.
 */
static void entry_insert(struct cardsight_file_list *list, const struct vfs_dirent *item)
{
	struct cardsight_file_entry entry;
	size_t position = list->count;

	snprintf(entry.name, sizeof(entry.name), "%s", item->name);
	entry.size = item->st.size;
	entry.dir = item->st.dir;
	while (position > 0U) {
		const struct cardsight_file_entry *previous = &list->entries[position - 1U];
		bool ordered = previous->dir != entry.dir ? previous->dir :
			strcasecmp(previous->name, entry.name) <= 0;

		if (ordered) {
			break;
		}
		list->entries[position] = list->entries[position - 1U];
		position--;
	}
	list->entries[position] = entry;
	list->count++;
}

/**
 * @brief Read one directory into the listing.
 *
 * @details The mount target and the relative path are joined into a full VFS
 * path. Every entry except "." and ".." counts towards the total; only the
 * first entries that fit the array are inserted, and the rest are reported as
 * truncated. An error from readdir stops the walk and is returned after the
 * directory is closed.
 *
 * @param[in]     target Mount target of the partition.
 * @param[in]     path   Directory path relative to the partition root.
 * @param[in,out] list   Listing to fill.
 * @return 0 on success (possibly truncated), or a negative errno.
 */
static int list_directory(const char *target, const char *path, struct cardsight_file_list *list)
{
	char full[VFS_PATH_MAX];
	struct vfs_dir *dir;
	int error = 0;
	int result;

	if (strcmp(path, "/") == 0) {
		result = snprintf(full, sizeof(full), "%s", target);
	} else {
		result = snprintf(full, sizeof(full), "%s%s", target, path);
	}
	if (result <= 0 || (size_t)result >= sizeof(full)) {
		return -ENAMETOOLONG;
	}
	dir = vfs_opendir(full, &error);
	if (dir == NULL) {
		return error != 0 ? error : -EIO;
	}
	for (;;) {
		struct vfs_dirent item;

		result = vfs_readdir(dir, &item);
		if (result == -ENOENT) {
			break;
		}
		if (result != 0) {
			error = result;
			break;
		}
		if (strcmp(item.name, ".") == 0 || strcmp(item.name, "..") == 0) {
			continue;
		}
		list->total++;
		if (list->count < ARRAY_SIZE(list->entries)) {
			entry_insert(list, &item);
		}
	}
	vfs_closedir(dir);
	list->truncated = list->total > list->count;
	return error != 0 ? error : 0;
}

/**
 * @brief List one directory of a partition into the shared file list.
 *
 * @details The listing is built in a temporary heap object so the shared state
 * is only written once the walk has finished. The partition is mounted
 * read-only at "/files" when nothing has it mounted yet, and unmounted again
 * before the result is published. Every outcome, including "no memory" and
 * "bad path", publishes a listing that carries the error, because the page
 * shows the reason instead of a banner; the function itself always returns 0.
 *
 * @param[in] partition Partition number, one-based.
 * @param[in] path      Directory path relative to the partition root.
 * @return Always 0; the error is stored in @c app.files.
 */
int card_list_files(uint32_t partition, const char *path)
{
	char block[VFS_BLK_NAME];
	char target[VFS_NAME_MAX + 1];
	struct cardsight_file_list *list;
	bool temporary = false;
	int result;

	if (partition == 0U || path == NULL || !path_safe(path)) {
		result = -EINVAL;
		goto store;
	}
	list = pvPortMalloc(sizeof(*list));
	if (list == NULL) {
		result = -ENOMEM;
		goto store;
	}
	memset(list, 0, sizeof(*list));
	snprintf(block, sizeof(block), "%sp%u", CONFIG_STORAGE_PROXY_SOURCE_DISK, partition);
	if (mounted_target(block, target, sizeof(target)) != 0) {
		result = vfs_mount_ex(block, "/files", NULL, VFS_MOUNT_READ_ONLY);
		if (result != 0) {
			vPortFree(list);
			goto store;
		}
		snprintf(target, sizeof(target), "/files");
		temporary = true;
	}
	result = list_directory(target, path, list);
	if (temporary) {
		(void)vfs_umount(target);
	}
	list->partition = partition;
	snprintf(list->path, sizeof(list->path), "%s", path);
	list->error = result;
	list->valid = true;
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	app.files = *list;
	mutex_unlock(&app_lock);
	vPortFree(list);
	return 0;
store:
	/* The page shows the reason instead of a banner; the directory is simply unavailable. */
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	memset(&app.files, 0, sizeof(app.files));
	app.files.partition = partition;
	if (path != NULL) {
		snprintf(app.files.path, sizeof(app.files.path), "%s", path);
	}
	app.files.error = result;
	app.files.valid = true;
	mutex_unlock(&app_lock);
	return 0;
}
