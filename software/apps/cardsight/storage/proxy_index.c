// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_index.c
 * @brief The index of what the volume shows: nodes, names, directory slots and their lookups.
 *
 * Nodes form a parent/child/sibling tree in a flat array. Names and source
 * paths live in one string arena addressed by offset, and directory slots are
 * handed out by the host, so the index records them and retires whatever a
 * reused slot left behind.
 */

#include "proxy_internal.h"


/**
 * @brief Compare two nodes by first cluster for the sorted range index.
 *
 * @param[in] left  First element, a node index.
 * @param[in] right Second element, a node index.
 * @return Negative, zero or positive as @p left sorts before, with or after @p right.
 */
static int proxy_range_compare(const void *left, const void *right)
{
	uint32_t left_index = *(const uint32_t *)left;
	uint32_t right_index = *(const uint32_t *)right;
	uint32_t left_cluster = proxy.nodes[left_index].first_cluster;
	uint32_t right_cluster = proxy.nodes[right_index].first_cluster;

	return left_cluster < right_cluster ? -1 : left_cluster > right_cluster ? 1 : 0;
}


/**
 * @brief Rebuild the cluster-sorted node index used by cluster lookups.
 *
 * @details Every node that owns clusters and is not deleted is collected and
 * sorted by its first cluster, so a cluster number can later be resolved with
 * a binary search.
 */
void proxy_rebuild_ranges(void)
{
	proxy.range_count = 0U;
	for (uint32_t index = 0U; index < proxy.node_count; index++) {
		if (proxy.nodes[index].cluster_count != 0U && !proxy.nodes[index].deleted) {
			proxy.range_nodes[proxy.range_count++] = index;
		}
	}
	qsort(proxy.range_nodes, proxy.range_count, sizeof(*proxy.range_nodes), proxy_range_compare);
}


/**
 * @brief Copy a string into the arena and return its offset.
 *
 * @details The arena only grows upwards, so an object that is renamed or
 * reclassified leaks its old copy rather than compacting the arena; running
 * out of arena marks the view truncated.
 *
 * @param[in] string String to copy.
 * @return Offset in the arena, or UINT32_MAX when it does not fit.
 */
uint32_t proxy_store_string(const char *string)
{
	size_t size = strlen(string) + 1U;
	uint32_t offset;

	if (size > CONFIG_STORAGE_PROXY_STRING_BYTES - proxy.string_used) {
		return UINT32_MAX;
	}
	offset = proxy.string_used;
	memcpy(proxy.strings + offset, string, size);
	proxy.string_used += size;
	return offset;
}


/**
 * @brief Make a name safe to use as a source file name.
 *
 * @details Control characters and the characters a FAT/exFAT source cannot
 * hold are replaced with '_'. A trailing space or dot is also replaced, and an
 * empty result becomes "_", so the name is never invalid and never empty.
 *
 * @param[in]  input  Name to sanitise.
 * @param[out] output Buffer receiving the sanitised name.
 * @param[in]  size   Size of @p output in bytes.
 */
static void proxy_sanitize_name(const char *input, char *output, size_t size)
{
	/** @brief Characters a source file name cannot contain. */
	static const char invalid[] = "\"*/:<>?\\|";
	size_t length = 0U;

	while (*input != '\0' && length + 1U < size) {
		unsigned char value = (unsigned char)*input++;

		if (value < 0x20U || strchr(invalid, value) != NULL) {
			value = '_';
		}
		output[length++] = (char)value;
	}
	while (length != 0U && (output[length - 1U] == ' ' || output[length - 1U] == '.')) {
		output[length - 1U] = '_';
		break;
	}
	if (length == 0U) {
		output[length++] = '_';
	}
	output[length] = '\0';
}


/**
 * @brief Report whether a live child with a name exists.
 *
 * @param[in] parent Parent node index.
 * @param[in] name   Name to compare case-insensitively.
 * @return True when a live child has that name.
 */
static bool proxy_name_exists(uint32_t parent, const char *name)
{
	uint32_t child = proxy.nodes[parent].first_child;

	while (child != PROXY_INVALID_NODE) {
		if (!proxy.nodes[child].retired && !proxy.nodes[child].tombstone &&
			strcasecmp(proxy_string(proxy.nodes[child].name_offset), name) == 0) {
			return true;
		}
		child = proxy.nodes[child].next_sibling;
	}
	return false;
}


/**
 * @brief Produce a name that no live sibling uses.
 *
 * @details The sanitised base name is tried first. When it is taken, "~N"
 * suffixes are appended and the base is shortened as needed to stay within
 * @p size, for up to ten thousand attempts. A synthetic "entry-XXXXXXXX" name
 * is the last resort so a node can always be created.
 *
 * @param[in]  parent Parent node index.
 * @param[in]  input  Requested name.
 * @param[out] output Buffer receiving the unique name.
 * @param[in]  size   Size of @p output in bytes.
 */
static void proxy_unique_name(uint32_t parent, const char *input, char *output, size_t size)
{
	char base[VFS_NAME_MAX + 1];

	proxy_sanitize_name(input, base, sizeof(base));
	if (!proxy_name_exists(parent, base)) {
		strncpy(output, base, size - 1U);
		output[size - 1U] = '\0';
		return;
	}
	for (uint32_t suffix = 1U; suffix < 10000U; suffix++) {
		int digits = snprintf(NULL, 0, "~%u", suffix);
		size_t keep = strlen(base);

		if ((size_t)digits + keep >= size) {
			keep = size - (size_t)digits - 1U;
		}
		snprintf(output, size, "%.*s~%u", (int)keep, base, suffix);
		if (!proxy_name_exists(parent, output)) {
			return;
		}
	}
	snprintf(output, size, "entry-%08x", proxy.node_count);
}


/**
 * @brief Number of directory slots a name occupies.
 *
 * @details UTF-8 bytes are an upper bound for UTF-16 code units, which leaves
 * stable slack for renames.
 *
 * @param[in] name UTF-8 name.
 * @return Slot count, including the stream and name entries.
 */
static uint32_t proxy_entry_span_name(const char *name)
{
	/* UTF-8 bytes are an upper bound for UTF-16 code units, leaving stable slack for renames. */
	return 2U + (uint32_t)((strlen(name) + 14U) / 15U);
}


/**
 * @brief Find the next free directory slot of a parent.
 *
 * @details The root starts after its three system entries; every other
 * directory starts at zero. The highest end of any child's slot span wins, so
 * a new object is appended behind everything the host already saw.
 *
 * @param[in] parent Parent node index.
 * @return First unused absolute slot.
 */
static uint32_t proxy_next_directory_slot(uint32_t parent)
{
	uint32_t slot = parent == 0U ? 3U : 0U;
	uint32_t child = proxy.nodes[parent].first_child;

	while (child != PROXY_INVALID_NODE) {
		uint32_t end = proxy.nodes[child].directory_slot +
			proxy_entry_span_name(proxy_string(proxy.nodes[child].name_offset));

		if (end > slot) {
			slot = end;
		}
		child = proxy.nodes[child].next_sibling;
	}
	return slot;
}


/**
 * @brief Add one node to the index.
 *
 * @details The name is made unique and stored in the arena together with the
 * source path. The node is appended to the parent's child list and gets the
 * next free directory slot. When the layout is already in use (the view runs
 * and clusters are being handed out) the node immediately receives contiguous
 * clusters; if that fails the node is rolled back. Reaching an index limit
 * marks the view truncated and yields PROXY_INVALID_NODE.
 *
 * @param[in] parent      Node index of the containing directory.
 * @param[in] name        Requested name; made unique and sanitised.
 * @param[in] directory   True for a directory.
 * @param[in] source      Where the node's data comes from.
 * @param[in] source_data Source path or text, or NULL.
 * @param[in] size        File size in bytes.
 * @return The new node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_add_node(uint32_t parent, const char *name, bool directory,
			       enum proxy_source source, const char *source_data, uint64_t size)
{
	struct proxy_node *node;
	char display[VFS_NAME_MAX + 1];
	uint32_t index;
	uint32_t name_offset;
	uint32_t source_offset = 0U;

	if (proxy.node_count >= CONFIG_STORAGE_PROXY_MAX_ENTRIES) {
		proxy.truncated = true;
		proxy_stats.index_full++;
		return PROXY_INVALID_NODE;
	}
	proxy_unique_name(parent, name, display, sizeof(display));
	name_offset = proxy_store_string(display);
	if (name_offset == UINT32_MAX) {
		proxy.truncated = true;
		proxy_stats.strings_full++;
		return PROXY_INVALID_NODE;
	}
	if (source_data != NULL) {
		source_offset = proxy_store_string(source_data);
		if (source_offset == UINT32_MAX) {
			proxy.truncated = true;
			proxy_stats.strings_full++;
			return PROXY_INVALID_NODE;
		}
	}
	index = proxy.node_count++;
	node = &proxy.nodes[index];
	memset(node, 0, sizeof(*node));
	node->parent = parent;
	node->first_child = PROXY_INVALID_NODE;
	node->last_child = PROXY_INVALID_NODE;
	node->next_sibling = PROXY_INVALID_NODE;
	node->directory_slot = proxy_next_directory_slot(parent);
	node->name_offset = name_offset;
	node->source_offset = source_offset;
	node->size = size;
	node->source = source;
	node->directory = directory;
	node->scanned = !directory || source != PROXY_SOURCE_VFS;
	if (proxy.layout_ready && proxy_allocate_node_clusters(index) != 0) {
		proxy.node_count--;
		proxy.truncated = true;
		return PROXY_INVALID_NODE;
	}
	if (proxy.nodes[parent].last_child == PROXY_INVALID_NODE) {
		proxy.nodes[parent].first_child = index;
	} else {
		proxy.nodes[proxy.nodes[parent].last_child].next_sibling = index;
	}
	proxy.nodes[parent].last_child = index;
	return index;
}


/**
 * @brief Add a read-only text file to a directory of the view.
 *
 * @param[in] parent  Node index of the containing directory.
 * @param[in] name    Name of the file inside the view.
 * @param[in] message Text the file returns when read.
 * @return 0 on success, or -ENOSPC when the index or arena is full.
 */
int proxy_add_message(uint32_t parent, const char *name, const char *message)
{
	return proxy_add_node(parent, name, false, PROXY_SOURCE_MEMORY,
			      message, strlen(message)) == PROXY_INVALID_NODE ? -ENOSPC : 0;
}


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
 * @brief Find a live child by name.
 *
 * @details Retired nodes and tombstones are skipped, because they must not
 * appear in a directory or be matched a second time.
 *
 * @param[in] parent Parent node index.
 * @param[in] name   Name to look for, compared case-insensitively.
 * @return The child index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_find_child(uint32_t parent, const char *name)
{
	uint32_t child = proxy.nodes[parent].first_child;

	while (child != PROXY_INVALID_NODE) {
		if (!proxy.nodes[child].retired && !proxy.nodes[child].tombstone &&
			strcasecmp(proxy_string(proxy.nodes[child].name_offset), name) == 0) {
			return child;
		}
		child = proxy.nodes[child].next_sibling;
	}
	return PROXY_INVALID_NODE;
}


/**
 * @brief Find a live node by its first cluster.
 *
 * @details A linear scan of the index; cluster zero never matches, and retired
 * nodes and tombstones are skipped.
 *
 * @param[in] cluster First cluster to look for.
 * @return The node index, or PROXY_INVALID_NODE.
 */
uint32_t proxy_find_cluster(uint32_t cluster)
{
	for (uint32_t index = 1U; index < proxy.node_count; index++) {
		if (proxy.nodes[index].first_cluster == cluster && cluster != 0U && !proxy.nodes[index].retired &&
			!proxy.nodes[index].tombstone) {
			return index;
		}
	}
	return PROXY_INVALID_NODE;
}


/**
 * @brief Find a node at a cluster that is not deleted.
 *
 * @details As above, but only an object that is still alive; the host may hand
 * a freed cluster to a new file.
 *
 * @param[in] cluster First cluster to look for.
 * @return The node index, or PROXY_INVALID_NODE.
 */
static uint32_t proxy_find_live_cluster(uint32_t cluster)
{
	uint32_t index = proxy_find_cluster(cluster);

	return index != PROXY_INVALID_NODE && !proxy.nodes[index].deleted ? index : PROXY_INVALID_NODE;
}


/**
 * @brief Find a child that is not deleted.
 *
 * @param[in] parent Parent node index.
 * @param[in] name   Name to look for.
 * @return The child index, or PROXY_INVALID_NODE.
 */
static uint32_t proxy_find_live_child(uint32_t parent, const char *name)
{
	uint32_t index = proxy_find_child(parent, name);

	return index != PROXY_INVALID_NODE && !proxy.nodes[index].deleted ? index : PROXY_INVALID_NODE;
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


/**
 * @brief Build the source path of a child of a source-backed directory.
 *
 * @details The parent's source path and the child name are joined, avoiding a
 * double slash when the parent is the source root.
 *
 * @param[in]  parent Parent node index.
 * @param[in]  name   Child name.
 * @param[out] path   Buffer receiving the path.
 * @return 0 on success, -ENODEV when the parent has no source, or -ENAMETOOLONG.
 */
int proxy_child_path(uint32_t parent, const char *name, char path[VFS_PATH_MAX])
{
	const struct proxy_node *parent_node = &proxy.nodes[parent];
	const char *base;
	int length;

	if (parent_node->source != PROXY_SOURCE_VFS) {
		return -ENODEV;
	}
	base = proxy_string(parent_node->source_offset);
	length = snprintf(path, VFS_PATH_MAX, strcmp(base, "/") == 0 ? "/%s" : "%s/%s", base, name);
	return length < 0 || length >= VFS_PATH_MAX ? -ENAMETOOLONG : 0;
}


/**
 * @brief Move a node under a new parent and give it a fresh directory slot.
 *
 * @details The node is unlinked from the old parent's child list, which also
 * recomputes the old parent's last-child pointer when it pointed at the node,
 * and is appended to the new parent with the next free slot there.
 *
 * @param[in] node_index Node to move.
 * @param[in] parent     New parent node index.
 */
void proxy_reparent(uint32_t node_index, uint32_t parent)
{
	struct proxy_node *node = &proxy.nodes[node_index];
	struct proxy_node *old_parent;
	struct proxy_node *new_parent;
	uint32_t *link;

	if (node->parent == parent) {
		return;
	}
	old_parent = &proxy.nodes[node->parent];
	link = &old_parent->first_child;
	while (*link != PROXY_INVALID_NODE && *link != node_index) {
		link = &proxy.nodes[*link].next_sibling;
	}
	if (*link == node_index) {
		*link = node->next_sibling;
		if (old_parent->last_child == node_index) {
			old_parent->last_child = PROXY_INVALID_NODE;
			for (uint32_t child = old_parent->first_child; child != PROXY_INVALID_NODE;
			     child = proxy.nodes[child].next_sibling) {
				old_parent->last_child = child;
			}
		}
	}
	new_parent = &proxy.nodes[parent];
	node->parent = parent;
	node->directory_slot = proxy_next_directory_slot(parent);
	node->next_sibling = PROXY_INVALID_NODE;
	if (new_parent->last_child == PROXY_INVALID_NODE) {
		new_parent->first_child = node_index;
	} else {
		proxy.nodes[new_parent->last_child].next_sibling = node_index;
	}
	new_parent->last_child = node_index;
}


/**
 * @brief Leave a deleted placeholder that keeps the slots of a moved node.
 *
 * @details The host chooses where an entry goes and reuses the slots of
 * deleted ones, so the node takes the host's slot. A deleted node that held
 * any of those slots is retired, or both would claim them. The placeholder is
 * a copy of the node that is marked deleted, tombstone and never scanned; it
 * is appended to the same parent so those slots stay occupied. An exhausted
 * index marks the view truncated instead.
 *
 * @param[in] node_index Node whose slots must stay occupied.
 */
void proxy_leave_tombstone(uint32_t node_index)
{
	const struct proxy_node *node = &proxy.nodes[node_index];
	struct proxy_node *parent = &proxy.nodes[node->parent];
	struct proxy_node *tombstone;
	uint32_t index;

	if (proxy.node_count >= CONFIG_STORAGE_PROXY_MAX_ENTRIES) {
		proxy.truncated = true;
		return;
	}
	index = proxy.node_count++;
	tombstone = &proxy.nodes[index];
	memset(tombstone, 0, sizeof(*tombstone));
	tombstone->parent = node->parent;
	tombstone->first_child = PROXY_INVALID_NODE;
	tombstone->last_child = PROXY_INVALID_NODE;
	tombstone->next_sibling = PROXY_INVALID_NODE;
	tombstone->directory_slot = node->directory_slot;
	tombstone->name_offset = node->name_offset;
	tombstone->directory = node->directory;
	tombstone->size = node->size;
	tombstone->source = PROXY_SOURCE_NONE;
	tombstone->scanned = true;
	tombstone->deleted = true;
	tombstone->tombstone = true;
	if (parent->last_child == PROXY_INVALID_NODE) {
		parent->first_child = index;
	} else {
		proxy.nodes[parent->last_child].next_sibling = index;
	}
	parent->last_child = index;
}


/**
 * @brief Take the host's directory slot and retire whatever held it before.
 *
 * @details Every live sibling whose slot span overlaps the new span is marked
 * deleted and retired, so only one object can claim each slot of the view.
 *
 * @param[in] node_index Node taking the slot.
 * @param[in] slot       Absolute first slot of the host entry.
 */
void proxy_adopt_slot(uint32_t node_index, uint32_t slot)
{
	struct proxy_node *node = &proxy.nodes[node_index];
	uint32_t entries = proxy_object_entries(node);

	node->directory_slot = slot;
	for (uint32_t child = proxy.nodes[node->parent].first_child; child != PROXY_INVALID_NODE;
	     child = proxy.nodes[child].next_sibling) {
		struct proxy_node *other = &proxy.nodes[child];
		uint32_t other_entries;

		if (child == node_index || other->retired) {
			continue;
		}
		other_entries = proxy_object_entries(other);
		if (other->directory_slot < slot + entries && slot < other->directory_slot + other_entries) {
			other->deleted = true;
			other->retired = true;
		}
	}
}
