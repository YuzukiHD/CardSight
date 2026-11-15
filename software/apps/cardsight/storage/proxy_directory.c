// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_directory.c
 * @brief exFAT directory entries: reading the view, parsing host writes, and syncing back.
 *
 * The view has no on-disk directory: a sector is synthesized from the index
 * nodes whose directory slots fall into it. In the other direction, each
 * directory sector the host writes is parsed here and the entries it holds are
 * matched to nodes or created as new ones.
 */

#include "proxy_internal.h"


/**
 * @brief Convert a UTF-8 string to UTF-16 for an exFAT name.
 *
 * @details Decodes one code point at a time, replacing malformed sequences
 * with U+FFFD. Code points above the BMP are written as a surrogate pair, and
 * the conversion stops when the output capacity would be exceeded.
 *
 * @param[in]  input    NUL-terminated UTF-8 string.
 * @param[out] output   Buffer receiving UTF-16 code units.
 * @param[in]  capacity Number of code units @p output can hold.
 * @return Number of code units written.
 */
static size_t proxy_utf16(const char *input, uint16_t *output, size_t capacity)
{
	size_t length = 0U;

	while (*input != '\0' && length < capacity) {
		uint32_t codepoint;
		unsigned char first = (unsigned char)*input++;

		if (first < 0x80U) {
			codepoint = first;
		} else if ((first & 0xe0U) == 0xc0U && ((unsigned char)input[0] & 0xc0U) == 0x80U) {
			codepoint = ((uint32_t)(first & 0x1fU) << 6) | ((unsigned char)*input++ & 0x3fU);
		} else if ((first & 0xf0U) == 0xe0U && ((unsigned char)input[0] & 0xc0U) == 0x80U &&
			   ((unsigned char)input[1] & 0xc0U) == 0x80U) {
			codepoint = ((uint32_t)(first & 0x0fU) << 12) |
				    ((uint32_t)((unsigned char)input[0] & 0x3fU) << 6) |
				    ((unsigned char)input[1] & 0x3fU);
			input += 2;
		} else if ((first & 0xf8U) == 0xf0U && ((unsigned char)input[0] & 0xc0U) == 0x80U &&
			   ((unsigned char)input[1] & 0xc0U) == 0x80U &&
			   ((unsigned char)input[2] & 0xc0U) == 0x80U) {
			codepoint = ((uint32_t)(first & 0x07U) << 18) |
				    ((uint32_t)((unsigned char)input[0] & 0x3fU) << 12) |
				    ((uint32_t)((unsigned char)input[1] & 0x3fU) << 6) |
				    ((unsigned char)input[2] & 0x3fU);
			input += 3;
		} else {
			codepoint = 0xfffdU;
		}
		if (codepoint > 0xffffU) {
			if (length + 2U > capacity) {
				break;
			}
			codepoint -= 0x10000U;
			output[length++] = 0xd800U | (codepoint >> 10);
			output[length++] = 0xdc00U | (codepoint & 0x3ffU);
		} else {
			output[length++] = (uint16_t)codepoint;
		}
	}
	return length;
}


/**
 * @brief Number of 32-byte directory entries a node occupies.
 *
 * @details An object always has a stream entry and a name entry, plus one name
 * entry for every 15 UTF-16 code units of its name.
 *
 * @param[in] node Node to measure.
 * @return Entry count, including the stream and name entries.
 */
uint32_t proxy_object_entries(const struct proxy_node *node)
{
	uint16_t name[VFS_NAME_MAX + 1];
	size_t length = proxy_utf16(proxy_string(node->name_offset), name, VFS_NAME_MAX);

	return 2U + (uint32_t)((length + 14U) / 15U);
}


/**
 * @brief Compute the exFAT name hash stored in a stream entry.
 *
 * @details The name is upper-cased one code unit at a time and each byte is
 * folded into a rotating 16-bit checksum, low byte first, as the exFAT
 * specification defines it.
 *
 * @param[in] name   UTF-16 name.
 * @param[in] length Number of code units in @p name.
 * @return The name hash.
 */
static uint16_t proxy_name_hash(const uint16_t *name, size_t length)
{
	uint16_t checksum = 0U;

	for (size_t index = 0U; index < length; index++) {
		uint16_t character = (uint16_t)ff_wtoupper(name[index]);

		checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
			(checksum >> 1) + (uint8_t)character);
		checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
			(checksum >> 1) + (uint8_t)(character >> 8));
	}
	return checksum;
}


/**
 * @brief Build one raw exFAT directory entry of a node.
 *
 * @details Entry zero is the stream entry (attribute and the read-only bit for
 * the view or a memory-backed file), entry one is the name entry with length,
 * hash, first cluster and both data lengths, and the remaining entries are the
 * name characters in 15-code-unit chunks. Unknown fields stay zero.
 *
 * @param[in]  node     Node to render.
 * @param[in]  position Entry position inside the object's group.
 * @param[out] entry    Buffer receiving the 32-byte entry.
 */
static void proxy_object_entry_raw(const struct proxy_node *node, uint32_t position,
				   uint8_t entry[32])
{
	uint16_t name[VFS_NAME_MAX + 1];
	size_t length = proxy_utf16(proxy_string(node->name_offset), name, VFS_NAME_MAX);
	uint32_t entries = 2U + (uint32_t)((length + 14U) / 15U);
	uint64_t data_length = node->directory ?
		(uint64_t)node->cluster_count * PROXY_CLUSTER_SIZE : node->size;

	memset(entry, 0, 32U);
	if (position == 0U) {
		entry[0] = 0x85U;
		entry[1] = (uint8_t)(entries - 1U);
		put_le16(entry + 4U, (node->directory ? 0x10U : 0x20U) |
			(proxy.read_only || node->source == PROXY_SOURCE_MEMORY ? 0x01U : 0U));
	} else if (position == 1U) {
		entry[0] = 0xc0U;
		entry[1] = 0x03U;
		entry[3] = (uint8_t)length;
		put_le16(entry + 4U, proxy_name_hash(name, length));
		put_le64(entry + 8U, data_length);
		put_le32(entry + 20U, node->first_cluster);
		put_le64(entry + 24U, data_length);
	} else {
		size_t start = (size_t)(position - 2U) * 15U;

		entry[0] = 0xc1U;
		for (uint32_t character = 0U; character < 15U; character++) {
			size_t name_index = start + character;

			put_le16(entry + 2U + character * 2U,
				 name_index < length ? name[name_index] : 0U);
		}
	}
}


/**
 * @brief Compute the checksum of an object's whole entry group.
 *
 * @details The stream entry's own checksum field (bytes 2 and 3) is skipped,
 * and every other byte of every entry is folded into the rotating checksum.
 *
 * @param[in] node Node whose group is checksummed.
 * @return The entry-set checksum.
 */
static uint16_t proxy_object_checksum(const struct proxy_node *node)
{
	uint8_t entry[32];
	uint16_t checksum = 0U;
	uint32_t entries = proxy_object_entries(node);

	for (uint32_t position = 0U; position < entries; position++) {
		proxy_object_entry_raw(node, position, entry);
		for (uint32_t index = 0U; index < sizeof(entry); index++) {
			if (position == 0U && (index == 2U || index == 3U)) {
				continue;
			}
			checksum = (uint16_t)(((checksum & 1U) != 0U ? 0x8000U : 0U) +
				(checksum >> 1) + entry[index]);
		}
	}
	return checksum;
}


/**
 * @brief Build one directory entry of a node, with its checksum filled in.
 *
 * @details The raw entry is generated first, and the checksum is only patched
 * into the stream entry, because computing it requires the other entries.
 *
 * @param[in]  node     Node to render.
 * @param[in]  position Entry position inside the object's group.
 * @param[out] entry    Buffer receiving the 32-byte entry.
 */
static void proxy_object_entry(const struct proxy_node *node, uint32_t position,
			       uint8_t entry[32])
{
	proxy_object_entry_raw(node, position, entry);
	if (position == 0U) {
		put_le16(entry + 2U, proxy_object_checksum(node));
	}
}


/**
 * @brief Render one of the three fixed system entries of the root directory.
 *
 * @details Position zero is the volume label "CARDSIGHT", position one is the
 * allocation bitmap and position two is the up-case table; each carries the
 * cluster and length recorded in the view.
 *
 * @param[in]  position System entry index.
 * @param[out] entry    Buffer receiving the 32-byte entry.
 */
static void proxy_root_system_entry(uint32_t position, uint8_t entry[32])
{
	/** @brief Volume label stored in the root directory. */
	static const uint16_t label[] = { 'C', 'A', 'R', 'D', 'S', 'I', 'G', 'H', 'T' };

	memset(entry, 0, 32U);
	if (position == 0U) {
		entry[0] = 0x83U;
		entry[1] = sizeof(label) / sizeof(label[0]);
		for (uint32_t index = 0U; index < sizeof(label) / sizeof(label[0]); index++) {
			put_le16(entry + 2U + index * 2U, label[index]);
		}
	} else if (position == 1U) {
		entry[0] = 0x81U;
		put_le32(entry + 20U, proxy.bitmap_cluster);
		put_le64(entry + 24U, proxy.bitmap_bytes);
	} else {
		entry[0] = 0x82U;
		put_le32(entry + 4U, proxy.upcase_checksum);
		put_le32(entry + 20U, proxy.upcase_cluster);
		put_le64(entry + 24U, proxy.upcase_bytes);
	}
}


/**
 * @brief Render one entry of a directory by its absolute slot number.
 *
 * @details The root directory reserves slots 0 to 2 for its system entries.
 * For every other directory the child list is walked and the node whose slot
 * range covers @p wanted supplies the entry. A deleted node is rendered with
 * its in-use bit cleared, which is how exFAT marks a deleted entry.
 *
 * @param[in]  directory Directory node.
 * @param[in]  wanted    Absolute slot number inside the directory.
 * @param[out] entry     Buffer receiving the 32-byte entry.
 * @return True when the slot belongs to an object.
 */
static bool proxy_directory_entry(const struct proxy_node *directory,
				  uint32_t wanted, uint8_t entry[32])
{
	uint32_t child;

	if (directory == &proxy.nodes[0] && wanted < 3U) {
		proxy_root_system_entry(wanted, entry);
		return true;
	}
	child = directory->first_child;
	while (child != PROXY_INVALID_NODE) {
		struct proxy_node *node = &proxy.nodes[child];
		uint32_t entries = proxy_object_entries(node);

		if (!node->retired && wanted >= node->directory_slot && wanted < node->directory_slot + entries) {
			proxy_object_entry(node, wanted - node->directory_slot, entry);
			if (node->deleted) {
				entry[0] &= 0x7fU;
			}
			return true;
		}
		child = node->next_sibling;
	}
	return false;
}


/**
 * @brief Render one sector of a directory.
 *
 * @details The children are indexed first, then each of the sixteen slots of
 * the sector is filled; slots that belong to no live object stay zero.
 *
 * @param[in]  node_index Node index of the directory.
 * @param[in]  offset     Byte offset of the sector inside the directory.
 * @param[out] sector     Buffer receiving the sector.
 * @return 0 on success, or a negative errno from indexing the children.
 */
int proxy_read_directory(uint32_t node_index,
				uint64_t offset, uint8_t sector[PROXY_SECTOR_SIZE])
{
	const struct proxy_node *directory;
	uint32_t first_entry = (uint32_t)(offset / 32U);
	int result = proxy_scan_directory(node_index);

	if (result != 0) {
		return result;
	}
	directory = &proxy.nodes[node_index];

	memset(sector, 0, PROXY_SECTOR_SIZE);
	for (uint32_t slot = 0U; slot < PROXY_SECTOR_SIZE / 32U; slot++) {
		(void)proxy_directory_entry(directory, first_entry + slot, sector + slot * 32U);
	}
	return 0;
}


/**
 * @brief Convert UTF-16 name code units back to UTF-8.
 *
 * @details Surrogates are encoded as three-byte sequences like any other code
 * unit, matching how the names were decoded. The output is always
 * NUL-terminated and stops before it would exceed @p capacity.
 *
 * @param[in]  input    UTF-16 code units.
 * @param[in]  length   Number of code units to convert.
 * @param[out] output   Buffer receiving the UTF-8 string.
 * @param[in]  capacity Size of @p output in bytes.
 * @return Number of bytes written, excluding the terminator.
 */
static size_t proxy_utf8(const uint16_t *input, size_t length, char *output, size_t capacity)
{
	size_t written = 0U;

	for (size_t index = 0U; index < length && input[index] != 0U; index++) {
		uint32_t codepoint = input[index];
		uint8_t bytes[3];
		size_t count;

		if (codepoint < 0x80U) {
			bytes[0] = (uint8_t)codepoint;
			count = 1U;
		} else if (codepoint < 0x800U) {
			bytes[0] = 0xc0U | (uint8_t)(codepoint >> 6);
			bytes[1] = 0x80U | (uint8_t)(codepoint & 0x3fU);
			count = 2U;
		} else {
			bytes[0] = 0xe0U | (uint8_t)(codepoint >> 12);
			bytes[1] = 0x80U | (uint8_t)((codepoint >> 6) & 0x3fU);
			bytes[2] = 0x80U | (uint8_t)(codepoint & 0x3fU);
			count = 3U;
		}
		if (written + count + 1U > capacity) {
			break;
		}
		memcpy(output + written, bytes, count);
		written += count;
	}
	output[written] = '\0';
	return written;
}


/**
 * @brief Apply one host directory entry to the index.
 *
 * @details Three cases are handled:
 * - An unknown name: the source object is created (mkdir, or an empty file with
 *   O_EXCL). If the name already exists on the card because an earlier delete
 *   was not flushed yet, the pending writes and deletes are carried out and the
 *   create is retried once. The node is then added unallocated and adopts the
 *   host's slot.
 * - A known object that moved: a tombstone keeps its old slots, its source
 *   file is renamed and the node is reparented under the new name.
 * - A known object whose size changed: the source file is truncated to the new
 *   length before the extent is updated.
 * Finally the extent is adopted and the staged data of the clusters is claimed.
 *
 * @param[in] parent        Parent node index.
 * @param[in] name          Entry name.
 * @param[in] directory     True when the entry is a directory.
 * @param[in] first_cluster First cluster from the host entry.
 * @param[in] size          Data length from the host entry.
 * @param[in] host_slot     Absolute slot the host entry starts at.
 * @return 0 on success, or a negative errno.
 */
static int proxy_sync_entry_inner(uint32_t parent, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, uint32_t host_slot)
{
	uint32_t node_index = proxy_identify_entry(parent, name, first_cluster, size);
	char path[VFS_PATH_MAX];
	int result = 0;

	if (node_index == PROXY_INVALID_NODE) {
		enum proxy_source source = proxy.nodes[parent].source == PROXY_SOURCE_VFS ?
			PROXY_SOURCE_VFS : PROXY_SOURCE_VOLATILE;

		if (source == PROXY_SOURCE_VFS) {
			result = proxy_child_path(parent, name, path);
			for (uint32_t attempt = 0U; result == 0 && attempt < 2U; attempt++) {
				if (directory) {
					result = vfs_mkdir(path);
				} else {
					int descriptor = vfs_open(path, VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL);

					result = descriptor < 0 ? descriptor : vfs_close(descriptor);
				}
				if (result != -EEXIST || attempt != 0U) {
					break;
				}
				/*
				 * The name was deleted and its slots reused before the host flushed, so the old object is
				 * still on the card. Carry out the deletes that are waiting, then create it again.
				 */
				result = proxy_write_flush();
				if (result == 0) {
					result = proxy_apply_deletes();
				}
				if (result != 0) {
					break;
				}
				/* the next pass creates it again */
			}
			proxy_stats.stage = 2U;
			if (result != 0) {
				/* A source file that exists but has no live node: say which hidden node holds the name. */
				uint32_t hidden = PROXY_INVALID_NODE;

				for (uint32_t child = proxy.nodes[parent].first_child; child != PROXY_INVALID_NODE;
				     child = proxy.nodes[child].next_sibling) {
					if (strcasecmp(proxy_string(proxy.nodes[child].name_offset), name) == 0) {
						hidden = child;
					}
				}
				pr_warn("proxy: cannot create at slot %u cluster %u: %d, hidden node %d (retired %d tombstone %d deleted %d slot %u)\n",
					host_slot, first_cluster, result, hidden == PROXY_INVALID_NODE ? -1 : (int)hidden,
					hidden != PROXY_INVALID_NODE && proxy.nodes[hidden].retired,
					hidden != PROXY_INVALID_NODE && proxy.nodes[hidden].tombstone,
					hidden != PROXY_INVALID_NODE && proxy.nodes[hidden].deleted,
					hidden != PROXY_INVALID_NODE ? proxy.nodes[hidden].directory_slot : 0U);
				return result;
			}
		}
		node_index = proxy_add_unallocated(parent, name, directory, source,
			source == PROXY_SOURCE_VFS ? path : NULL, size);
		if (node_index == PROXY_INVALID_NODE) {
			/* The index or arena is full; the volume stays mounted. */
			return -ENOSPC;
		}
		proxy_adopt_slot(node_index, host_slot);
	} else {
		struct proxy_node *node = &proxy.nodes[node_index];

		if (node->directory_slot != host_slot || node->parent != parent) {
			proxy_leave_tombstone(node_index);
			node = &proxy.nodes[node_index];
		}
		node->deleted = false;
		if (strcasecmp(proxy_string(node->name_offset), name) != 0 || node->parent != parent) {
			if (node->source == PROXY_SOURCE_VFS) {
				result = proxy_child_path(parent, name, path);
				if (result == 0) {
					proxy_stats.stage = 4U;
					result = proxy_write_flush();
				}
				if (result == 0) {
					result = vfs_rename(proxy_string(node->source_offset), path);
				}
				if (result != 0) {
					return result;
				}
				{
					uint32_t offset = proxy_store_string(path);

					if (offset == UINT32_MAX) {
						proxy.truncated = true;
						proxy_stats.strings_full++;
						return -ENOSPC;
					}
					node->source_offset = offset;
				}
			}
			{
				uint32_t offset = proxy_store_string(name);

				if (offset == UINT32_MAX) {
					proxy.truncated = true;
					proxy_stats.strings_full++;
					return -ENOSPC;
				}
				node->name_offset = offset;
			}
			proxy_reparent(node_index, parent);
		}
		proxy_adopt_slot(node_index, host_slot);
		if (node->source == PROXY_SOURCE_VFS && !directory && node->size != size) {
			int descriptor;

			pr_info("proxy: node %u size %llu -> %llu\n", node_index, (unsigned long long)node->size,
				(unsigned long long)size);
			proxy_drop_read_cache();
			proxy_stats.stage = 5U;
			descriptor = vfs_open(proxy_string(node->source_offset), VFS_O_WRITE);

			if (descriptor >= 0) {
				result = vfs_ftruncate(descriptor, size);
				(void)vfs_close(descriptor);
			}
			if (descriptor < 0 || result != 0) {
				return descriptor < 0 ? descriptor : result;
			}
		}
		node->size = size;
	}
	proxy_stats.stage = 6U;
	return proxy_set_extent(node_index, first_cluster, size == 0U && directory ? PROXY_CLUSTER_SIZE : size);
}


/**
 * @brief Apply one host directory entry and record the timing and failures.
 *
 * @details Wraps @c proxy_sync_entry_inner() with the timing counters, traces
 * the first forty file syncs to the log, and remembers the last error for the
 * status command.
 *
 * @param[in] parent        Parent node index.
 * @param[in] name          Entry name.
 * @param[in] directory     True when the entry is a directory.
 * @param[in] first_cluster First cluster from the host entry.
 * @param[in] size          Data length from the host entry.
 * @param[in] host_slot     Absolute slot the host entry starts at.
 * @return 0 on success, or a negative errno.
 */
static int proxy_sync_entry(uint32_t parent, const char *name, bool directory,
	uint32_t first_cluster, uint64_t size, uint32_t host_slot)
{
	uint64_t begin = proxy_time_begin();
	uint32_t pending_before = proxy.pending_count;
	int result;

	proxy_stats.stage = 0U;
	result = proxy_sync_entry_inner(parent, name, directory, first_cluster, size, host_slot);
	if (!directory && proxy_stats.sync_trace++ < 40U) {
		pr_info("proxy: sync slot %u cluster %u size %llu -> %d (stage %u), pending %u -> %u\n", host_slot, first_cluster,
			(unsigned long long)size, result, proxy_stats.stage, pending_before, proxy.pending_count);
	}
	if (result != 0) {
		proxy_stats.last_sync_error = result;
		pr_warn("proxy: entry sync failed: %d, directory %d, size %llu, stage %u\n", result, directory ? 1 : 0, (unsigned long long)size, proxy_stats.stage);
	}
	proxy_time_end(&proxy_stats.sync_entry, begin);
	return result;
}



/**
 * @brief Extract the name of a host entry from its secondary entries.
 *
 * @details The length lives in the stream entry, and the characters are
 * gathered from the following 0xC1 name entries, 15 code units each. The
 * gather stops at the requested length, and the entry is rejected when a
 * secondary entry is not a name entry or when fewer characters than the length
 * are available inside the window.
 *
 * @param[in]  sector      Window holding the entry group.
 * @param[in]  slot        Slot of the stream entry inside @p sector.
 * @param[in]  secondaries Number of secondary entries in the group.
 * @param[out] name        Buffer receiving the UTF-8 name.
 * @return True when a complete name was decoded.
 */
static bool proxy_entry_name(const uint8_t *sector, uint32_t slot, uint32_t secondaries,
	char name[VFS_NAME_MAX + 1])
{
	uint16_t characters[VFS_NAME_MAX];
	uint32_t length = sector[(slot + 1U) * 32U + 3U];
	uint32_t copied = 0U;

	if (length > VFS_NAME_MAX || slot + secondaries >= PROXY_WINDOW_SLOTS) {
		return false;
	}
	for (uint32_t entry = 2U; entry <= secondaries && copied < length; entry++) {
		const uint8_t *name_entry = sector + (slot + entry) * 32U;

		if (name_entry[0] != 0xc1U) {
			return false;
		}
		for (uint32_t character = 0U; character < 15U && copied < length; character++) {
			characters[copied++] = get_le16(name_entry + 2U + character * 2U);
		}
	}
	proxy_utf8(characters, copied, name, VFS_NAME_MAX + 1U);
	return copied == length;
}


/**
 * @brief Mark the neighbour entry the host deleted.
 *
 * @details The host clears the in-use bit of a stream entry when it deletes an
 * object. The object is found by cluster first and by name second, and only
 * the node that still lives at exactly this slot under this parent is marked:
 * the host may already have written the object's new entry before this one.
 *
 * @param[in] parent        Parent node index.
 * @param[in] sector        Contents of the previous sector.
 * @param[in] slot          Slot of the deleted entry inside @p sector.
 * @param[in] absolute_slot Absolute slot number of the deleted entry.
 */
static void proxy_mark_removed(uint32_t parent, const uint8_t *sector, uint32_t slot, uint32_t absolute_slot)
{
	uint32_t first_cluster;
	uint32_t node_index;
	uint32_t secondaries = sector[slot * 32U + 1U];
	char name[VFS_NAME_MAX + 1];

	if (slot + 1U >= PROXY_WINDOW_SLOTS || sector[(slot + 1U) * 32U] != 0xc0U) {
		return;
	}
	first_cluster = get_le32(sector + (slot + 1U) * 32U + 20U);
	node_index = proxy_find_cluster(first_cluster);
	if (node_index == PROXY_INVALID_NODE && proxy_entry_name(sector, slot, secondaries, name)) {
		node_index = proxy_find_child(parent, name);
	}
	/* Only if the node still lives there: the host may have written its new entry before this one. */
	if (node_index != PROXY_INVALID_NODE && proxy.nodes[node_index].directory_slot == absolute_slot &&
		proxy.nodes[node_index].parent == parent) {
		proxy.nodes[node_index].deleted = true;
	}
}


/**
 * @brief Parse one directory sector the host wrote and sync the entries it holds.
 *
 * @details The flow is:
 * 1. flush queued data so the directory is in order;
 * 2. read back the previous sector, because the host clears an entry's in-use
 *    bit in place and that delete must be seen;
 * 3. build a window of [kept head of a cut group][new sector], so an entry
 *    group straddling the sector edge can be completed;
 * 4. for each stream entry, note the deletes, skip groups that are unchanged
 *    copies of what the host was shown, and cut or sync the rest;
 * 5. rebuild the cluster index once the sector is processed.
 *
 * @param[in] parent Parent node index.
 * @param[in] offset Byte offset of the sector inside the directory.
 * @param[in] data   New sector contents.
 * @return 0 on success, or a negative errno.
 */
static int proxy_write_directory_inner(uint32_t parent, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE])
{
	/* [kept head of a cut group][sector written][sector after]; only this write can complete a cut group. */
	uint8_t *window = proxy.directory_window;
	uint8_t *previous = proxy.directory_previous;
	const uint32_t slots_per_sector = PROXY_SECTOR_SIZE / 32U;
	uint32_t head_slots = 0U;
	uint32_t window_slots;
	int result = proxy_write_flush();

	if (result != 0) {
		return result;
	}
	result = proxy_read_directory(parent, offset, previous);
	if (result != 0) {
		return result;
	}
	if (proxy.split_slots != 0U && proxy.split_parent == parent && proxy.split_offset + PROXY_SECTOR_SIZE == offset) {
		head_slots = proxy.split_slots;
		memcpy(window, proxy.split_head, (size_t)head_slots * 32U);
	}
	proxy.split_slots = 0U;
	memset(window + (size_t)head_slots * 32U, 0, sizeof(proxy.directory_window) - (size_t)head_slots * 32U);
	memcpy(window + (size_t)head_slots * 32U, data, PROXY_SECTOR_SIZE);
	window_slots = head_slots + slots_per_sector;
	for (uint32_t slot = 0U; slot < window_slots; slot++) {
		const uint8_t *entry = window + slot * 32U;
		const uint32_t local = slot - head_slots;

		if (slot >= head_slots && previous[local * 32U] == 0x85U && entry[0] == 0x05U) {
			proxy_mark_removed(parent, previous, local, (uint32_t)(offset / 32U) + local);
		}
		if (entry[0] == 0x85U) {
			uint32_t secondaries = entry[1];
			const uint8_t *stream;
			char name[VFS_NAME_MAX + 1];

			if (secondaries < 2U || secondaries > 18U) {
				continue;
			}
			if (slot + secondaries >= window_slots) {
				/* Cut by the sector edge: keep the head, the next sector write completes it. */
				proxy.split_slots = window_slots - slot;
				proxy.split_parent = parent;
				proxy.split_offset = offset;
				memcpy(proxy.split_head, window + slot * 32U, (size_t)proxy.split_slots * 32U);
				break;
			}
			if (window[(slot + 1U) * 32U] != 0xc0U || !proxy_entry_name(window, slot, secondaries, name)) {
				continue;
			}
			/* A neighbour the host rewrote with the sector: the same bytes as what it was shown, nothing to sync. */
			if (slot >= head_slots && slot - head_slots + secondaries < slots_per_sector &&
				memcmp(window + slot * 32U, previous + (slot - head_slots) * 32U, (size_t)(secondaries + 1U) * 32U) == 0) {
				slot += secondaries;
				continue;
			}
			stream = window + (slot + 1U) * 32U;
			{
				bool directory = (get_le16(entry + 4U) & 0x10U) != 0U;
				uint64_t data_length = get_le64(stream + 24U);
				uint64_t valid_length = get_le64(stream + 8U);

				/*
				 * A host reserves whole clusters in DataLength while writing; only ValidDataLength is data
				 * that exists. Taking the larger one would pad the source file with zeros.
				 */
				if (!directory && valid_length < data_length) {
					pr_info("proxy: valid length %llu below data length %llu\n",
						(unsigned long long)valid_length, (unsigned long long)data_length);
					data_length = valid_length;
				}
				result = proxy_sync_entry(parent, name, directory, get_le32(stream + 20U), data_length,
					(uint32_t)(offset / 32U) - head_slots + slot);
			}
			if (result != 0) {
				return result;
			}
			slot += secondaries;
		}
	}
	proxy_rebuild_ranges();
	return 0;
}


/**
 * @brief Parse one directory sector the host wrote, with directory timing.
 *
 * @param[in] parent Parent node index.
 * @param[in] offset Byte offset of the sector inside the directory.
 * @param[in] data   New sector contents.
 * @return 0 on success, or a negative errno.
 */
int proxy_write_directory(uint32_t parent, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE])
{
	uint64_t begin = proxy_time_begin();
	int result = proxy_write_directory_inner(parent, offset, data);
	proxy_time_end(&proxy_stats.directory_write, begin);
	return result;
}
