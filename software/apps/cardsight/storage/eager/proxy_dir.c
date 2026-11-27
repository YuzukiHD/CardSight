// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_dir.c
 * @brief exFAT directory entry rendering and reading the synthesized view.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

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
size_t proxy_utf16(const char *input, uint16_t *output, size_t capacity)
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
uint16_t proxy_name_hash(const uint16_t *name, size_t length)
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
void proxy_object_entry_raw(const struct proxy_node *node, uint32_t position,
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
uint16_t proxy_object_checksum(const struct proxy_node *node)
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
void proxy_object_entry(const struct proxy_node *node, uint32_t position,
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
void proxy_root_system_entry(uint32_t position, uint8_t entry[32])
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
bool proxy_directory_entry(const struct proxy_node *directory,
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
