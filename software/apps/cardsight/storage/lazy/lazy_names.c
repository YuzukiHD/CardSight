// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_names.c
 * @brief Name arena, UTF-16 name conversion, name hashing and the up-case table.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- names and persistent arena ---- */

/**
 * @brief Copy a directory name into the persistent arena.
 *
 * @param[in] name NUL-terminated name to copy.
 * @return Offset in the arena, or UINT32_MAX when the arena is full.
 */
uint32_t lazy_store_name(const char *name)
{
	size_t length = strlen(name) + 1U;
	uint32_t offset;

	if (lazy.name_arena == NULL || length > CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES - lazy.name_used) {
		return UINT32_MAX;
	}
	offset = lazy.name_used;
	memcpy(lazy.name_arena + offset, name, length);
	lazy.name_used += (uint32_t)length;
	return offset;
}

/**
 * @brief Resolve the persistent name of a directory.
 *
 * @param[in] identifier Directory identifier.
 * @return Pointer into the persistent name arena.
 */
const char *lazy_dir_name(uint32_t identifier)
{
	return lazy.name_arena + lazy.dirs[identifier].name_offset;
}

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
size_t lazy_utf16(const char *input, uint16_t *output, size_t capacity)
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
			output[length++] = (uint16_t)(0xd800U | (codepoint >> 10));
			output[length++] = (uint16_t)(0xdc00U | (codepoint & 0x3ffU));
		} else {
			output[length++] = (uint16_t)codepoint;
		}
	}
	return length;
}

/**
 * @brief Number of 32-byte slots a name occupies.
 *
 * @param[in] name UTF-8 name.
 * @return Slot count, including the stream and name entries.
 */
uint32_t lazy_name_entries(const char *name)
{
	uint16_t characters[VFS_NAME_MAX + 1];
	size_t length = lazy_utf16(name, characters, VFS_NAME_MAX);

	return 2U + (uint32_t)((length + 14U) / 15U);
}

/**
 * @brief Compute the exFAT name hash stored in a stream entry.
 *
 * @param[in] name   UTF-16 name.
 * @param[in] length Number of code units in @p name.
 * @return The name hash.
 */
uint16_t lazy_name_hash(const uint16_t *name, size_t length)
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

/* ---- up-case table ---- */

/**
 * @brief Build the up-case table the view advertises.
 *
 * @details Each UTF-16 code unit is mapped through the source's upper-case
 * conversion. Runs of 128 or more units that map to themselves are compressed
 * into the exFAT identity-run encoding (0xFFFF followed by the run length). The
 * checksum is computed over the bytes exactly as they are stored.
 *
 * @return 0 on success, or -ENOMEM.
 */
int lazy_build_upcase(void)
{
	uint16_t source = 0U;
	uint16_t character;
	uint32_t state = 0U;
	uint32_t run = 0U;
	uint32_t checksum = 0U;
	size_t offset = 0U;

	lazy.upcase = lazy_alloc(65536U * sizeof(uint16_t));
	if (lazy.upcase == NULL) {
		return -ENOMEM;
	}
	do {
		switch (state) {
		case 0:
			character = (uint16_t)ff_wtoupper(source);
			if (character != source) {
				source++;
				break;
			}
			for (run = 1U; (uint16_t)(source + run) != 0U &&
			     (uint16_t)(source + run) == (uint16_t)ff_wtoupper((uint16_t)(source + run)); run++) {
			}
			if (run >= 128U) {
				character = 0xffffU;
				state = 2U;
				break;
			}
			state = 1U;
			/* fall through */
		case 1:
			character = source++;
			if (--run == 0U) {
				state = 0U;
			}
			break;
		default:
			character = (uint16_t)run;
			source = (uint16_t)(source + run);
			state = 0U;
			break;
		}
		lazy.upcase[offset++] = (uint8_t)character;
		lazy.upcase[offset++] = (uint8_t)(character >> 8);
		checksum = lazy_checksum32(checksum, lazy.upcase[offset - 2U]);
		checksum = lazy_checksum32(checksum, lazy.upcase[offset - 1U]);
	} while (source != 0U);
	lazy.upcase_bytes = (uint32_t)offset;
	lazy.upcase_checksum = checksum;
	return 0;
}

/**
 * @brief Convert UTF-16 name code units back to UTF-8.
 *
 * @param[in]  input    UTF-16 code units.
 * @param[in]  length   Number of code units to convert.
 * @param[out] output   Buffer receiving the UTF-8 string.
 * @param[in]  capacity Size of @p output in bytes.
 * @return Number of bytes written, excluding the terminator.
 */
size_t lazy_utf8(const uint16_t *input, size_t length, char *output, size_t capacity)
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
 * @brief Extract the name of a host entry from its secondary entries.
 *
 * @param[in]  slots       Window slots.
 * @param[in]  slot        Slot of the stream entry inside the window.
 * @param[in]  secondaries Number of secondary entries in the group.
 * @param[out] name        Buffer receiving the UTF-8 name.
 * @return True when a complete name was decoded.
 */
bool lazy_entry_name(const uint8_t *slots, uint32_t slot, uint32_t secondaries,
	char name[VFS_NAME_MAX + 1])
{
	uint16_t characters[VFS_NAME_MAX];
	uint32_t length = slots[(slot + 1U) * 32U + 3U];
	uint32_t copied = 0U;

	if (length > VFS_NAME_MAX || slot + secondaries >= PROXY_LAZY_WINDOW_SLOTS) {
		return false;
	}
	for (uint32_t entry = 2U; entry <= secondaries && copied < length; entry++) {
		const uint8_t *name_entry = slots + (slot + entry) * 32U;

		if (name_entry[0] != 0xc1U) {
			return false;
		}
		for (uint32_t character = 0U; character < 15U && copied < length; character++) {
			characters[copied++] = (uint16_t)(name_entry[2U + character * 2U] |
				((uint16_t)name_entry[3U + character * 2U] << 8));
		}
	}
	lazy_utf8(characters, copied, name, VFS_NAME_MAX + 1U);
	return copied == length;
}
