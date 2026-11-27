// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_scan.c
 * @brief Mounting source partitions and indexing their directory trees.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/**
 * @brief Mount one source partition and add its folder to the root.
 *
 * @details The folder is always added under the name "partitionN", even when
 * the mount fails; in that case the folder receives an _ERROR.TXT message that
 * names the device and the error, so the host sees why it is empty. The mount
 * is read-only when the view is read-only.
 *
 * @param[in]  partition Zero-based partition index.
 * @param[out] root_node Node index of the folder that was added.
 * @return 0 on success, or a negative errno.
 */
int proxy_mount_partition(uint32_t partition, uint32_t *root_node)
{
	struct proxy_partition *part = &proxy.partitions[partition];
	char name[16];
	char message[192];
	int result;

	snprintf(name, sizeof(name), "partition%u", partition + 1U);
	*root_node = proxy_add_node(0U, name, true, PROXY_SOURCE_NONE, NULL, 0U);
	if (*root_node == PROXY_INVALID_NODE) {
		return -ENOSPC;
	}
	result = vfs_mount_ex(part->device, part->mount, NULL, proxy.read_only ? VFS_MOUNT_READ_ONLY : 0U);
	part->status = result;
	part->mounted = result == 0;
	if (result != 0) {
		snprintf(message, sizeof(message),
			 "CardSight could not mount %s (error %d).\r\n"
			 "Supported source formats: FAT/exFAT, ext2/3/4 and SquashFS.\r\n",
			 part->device, result);
		return proxy_add_message(*root_node, "_ERROR.TXT", message);
	}
	return 0;
}


/**
 * @brief Index the children of a source directory, once.
 *
 * @details The directory is opened on the mounted source and every entry is
 * added as a child, with its full source path. Leftover staging files from an
 * earlier run are unlinked instead of indexed. The directory is marked scanned
 * even when the walk stopped early, so it is not retried forever; a walk error
 * and a reached index limit are both reported to the host as small text files.
 *
 * @param[in] parent Node index of the directory.
 * @return 0 on success, or a negative errno.
 */
int proxy_scan_directory(uint32_t parent)
{
	struct proxy_node *parent_node = &proxy.nodes[parent];
	const char *parent_path;
	struct vfs_dirent *entry;
	struct vfs_dir *dir;
	int error = 0;

	if (parent_node->scanned) {
		return 0;
	}
	parent_path = proxy_string(parent_node->source_offset);
	entry = proxy_alloc(sizeof(*entry));
	if (entry == NULL) {
		return -ENOMEM;
	}
	dir = vfs_opendir(parent_path, &error);
	if (dir == NULL) {
		proxy_free(entry);
		return error;
	}
	while (!proxy.truncated && (error = vfs_readdir(dir, entry)) == 0) {
		char path[VFS_PATH_MAX];
		uint32_t child;
		int length;

		if (strcmp(parent_path, "/") == 0) {
			length = snprintf(path, sizeof(path), "/%s", entry->name);
		} else {
			length = snprintf(path, sizeof(path), "%s/%s", parent_path, entry->name);
		}
		if (length < 0 || (size_t)length >= sizeof(path)) {
			proxy.truncated = true;
			break;
		}
		if (strncmp(entry->name, PROXY_STAGE_PREFIX, strlen(PROXY_STAGE_PREFIX)) == 0) {
			/* Left by an earlier run that did not finish; it is not the card's data. */
			(void)vfs_unlink(path);
			continue;
		}
		child = proxy_add_node(parent, entry->name, entry->st.dir,
				       PROXY_SOURCE_VFS, path, entry->st.size);
		if (child == PROXY_INVALID_NODE) {
			break;
		}
	}
	vfs_closedir(dir);
	proxy_free(entry);
	parent_node->scanned = true;
	if (error != 0 && error != -ENOENT) {
		char message[96];

		snprintf(message, sizeof(message), "Directory indexing stopped with error %d.\r\n", error);
		(void)proxy_add_message(parent, "_INDEX_ERROR.TXT", message);
	}
	if (proxy.truncated && proxy_name_exists(0U, "_LIMITS.TXT") == false) {
		(void)proxy_add_message(0U, "_LIMITS.TXT",
			"The lazy proxy index reached its entry, path-memory, or path-length limit.\r\n");
	}
	return 0;
}


/**
 * @brief Match a host directory entry to a known node, or report that it is new.
 *
 * @details Which known object does a host entry describe? An object of that
 * name that is alive, else the live one at that cluster (a rename). A deleted
 * object counts as the one renamed only if it kept its size at the same
 * cluster; else the entry is a new file that reuses freed space, which may
 * revive a deleted object of the same name.
 *
 * @param[in] parent        Parent node index.
 * @param[in] name          Name from the host entry.
 * @param[in] first_cluster First cluster from the host entry.
 * @param[in] size          Data length from the host entry.
 * @return The matching node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_identify_entry(uint32_t parent, const char *name, uint32_t first_cluster, uint64_t size)
{
	uint32_t index = proxy_find_live_child(parent, name);

	if (index == PROXY_INVALID_NODE) {
		index = proxy_find_live_cluster(first_cluster);
	}
	if (index == PROXY_INVALID_NODE) {
		index = proxy_find_cluster(first_cluster);
		if (index != PROXY_INVALID_NODE && (!proxy.nodes[index].deleted || proxy.nodes[index].size != size)) {
			index = PROXY_INVALID_NODE;
		}
	}
	if (index == PROXY_INVALID_NODE) {
		index = proxy_find_child(parent, name);
	}
	return index;
}
