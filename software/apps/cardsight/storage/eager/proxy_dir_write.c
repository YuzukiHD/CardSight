// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_dir_write.c
 * @brief Parsing host directory writes and syncing them to the source.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

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
size_t proxy_utf8(const uint16_t *input, size_t length, char *output, size_t capacity)
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
int proxy_sync_entry_inner(uint32_t parent, const char *name, bool directory,
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
int proxy_sync_entry(uint32_t parent, const char *name, bool directory,
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
bool proxy_entry_name(const uint8_t *sector, uint32_t slot, uint32_t secondaries,
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
void proxy_mark_removed(uint32_t parent, const uint8_t *sector, uint32_t slot, uint32_t absolute_slot)
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
int proxy_write_directory_inner(uint32_t parent, uint64_t offset, const uint8_t data[PROXY_SECTOR_SIZE])
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
