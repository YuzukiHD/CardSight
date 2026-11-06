// SPDX-License-Identifier: GPL-2.0-or-later
/* The index of what the volume shows: nodes, names, directory slots and their lookups. */

#include "proxy_internal.h"


static int proxy_range_compare(const void *left, const void *right)
{
	uint32_t left_index = *(const uint32_t *)left;
	uint32_t right_index = *(const uint32_t *)right;
	uint32_t left_cluster = proxy.nodes[left_index].first_cluster;
	uint32_t right_cluster = proxy.nodes[right_index].first_cluster;

	return left_cluster < right_cluster ? -1 : left_cluster > right_cluster ? 1 : 0;
}


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


static void proxy_sanitize_name(const char *input, char *output, size_t size)
{
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


static uint32_t proxy_entry_span_name(const char *name)
{
	/* UTF-8 bytes are an upper bound for UTF-16 code units, leaving stable slack for renames. */
	return 2U + (uint32_t)((strlen(name) + 14U) / 15U);
}


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
		return PROXY_INVALID_NODE;
	}
	proxy_unique_name(parent, name, display, sizeof(display));
	name_offset = proxy_store_string(display);
	if (name_offset == UINT32_MAX) {
		proxy.truncated = true;
		return PROXY_INVALID_NODE;
	}
	if (source_data != NULL) {
		source_offset = proxy_store_string(source_data);
		if (source_offset == UINT32_MAX) {
			proxy.truncated = true;
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


int proxy_add_message(uint32_t parent, const char *name, const char *message)
{
	return proxy_add_node(parent, name, false, PROXY_SOURCE_MEMORY,
			      message, strlen(message)) == PROXY_INVALID_NODE ? -ENOMEM : 0;
}


int proxy_mount_partition(uint32_t partition, uint32_t *root_node)
{
	struct proxy_partition *part = &proxy.partitions[partition];
	char name[16];
	char message[192];
	int result;

	snprintf(name, sizeof(name), "partition%u", partition + 1U);
	*root_node = proxy_add_node(0U, name, true, PROXY_SOURCE_NONE, NULL, 0U);
	if (*root_node == PROXY_INVALID_NODE) {
		return -ENOMEM;
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


/* As above, but only an object that is still alive; the host may hand a freed cluster to a new file. */
static uint32_t proxy_find_live_cluster(uint32_t cluster)
{
	uint32_t index = proxy_find_cluster(cluster);

	return index != PROXY_INVALID_NODE && !proxy.nodes[index].deleted ? index : PROXY_INVALID_NODE;
}


static uint32_t proxy_find_live_child(uint32_t parent, const char *name)
{
	uint32_t index = proxy_find_child(parent, name);

	return index != PROXY_INVALID_NODE && !proxy.nodes[index].deleted ? index : PROXY_INVALID_NODE;
}


/*
 * Which known object does a host entry describe? An object of that name that is alive, else the live
 * one at that cluster (a rename). A deleted object counts as the one renamed only if it kept its size
 * at the same cluster; else the entry is a new file that reuses freed space, which may revive a deleted
 * object of the same name.
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


/*
 * The host chooses where an entry goes and reuses the slots of deleted ones, so the node takes the
 * host's slot. A deleted node that held any of those slots is retired, or both would claim them.
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
