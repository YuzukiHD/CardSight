// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file lazy_core.c
 * @brief Allocation, byte-order, checksum and callback guards shared by the lazy view.
 *
 * @details Part of the lazy exFAT proxy engine. This file only holds one
 * capability of the engine; the shared state lives in @c lazy_types.h and the
 * cross-module interfaces in @c lazy_private.h.
 */
#include "lazy_private.h"

/* ---- small helpers ---- */

/**
 * @brief Allocate memory for the view.
 *
 * @param[in] size Bytes to allocate.
 * @return The allocation, or NULL.
 */
void *lazy_alloc(size_t size)
{
#ifdef STORAGE_PROXY_HOST_TEST
	return malloc(size);
#else
	return pvPortMalloc(size);
#endif
}

/**
 * @brief Release memory obtained from @c lazy_alloc().
 *
 * @param[in] pointer Allocation to free, or NULL.
 */
void lazy_free(void *pointer)
{
	if (pointer == NULL) {
		return;
	}
#ifdef STORAGE_PROXY_HOST_TEST
	free(pointer);
#else
	vPortFree(pointer);
#endif
}

/**
 * @brief Allocate zeroed memory for the view.
 *
 * @param[in] count Number of elements.
 * @param[in] size  Size of one element.
 * @return The zeroed allocation, or NULL.
 */
void *lazy_calloc(size_t count, size_t size)
{
	void *pointer;

	if (count != 0U && size > SIZE_MAX / count) {
		return NULL;
	}
	pointer = lazy_alloc(count * size);
	if (pointer != NULL) {
		memset(pointer, 0, count * size);
	}
	return pointer;
}

/**
 * @brief Store a 16-bit value little endian.
 *
 * @param[out] destination Two bytes to write.
 * @param[in]  value       Value to store.
 */
void lazy_put_le16(void *destination, uint16_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/**
 * @brief Store a 32-bit value little endian.
 *
 * @param[out] destination Four bytes to write.
 * @param[in]  value       Value to store.
 */
void lazy_put_le32(void *destination, uint32_t value)
{
	uint8_t *bytes = destination;

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/**
 * @brief Store a 64-bit value little endian.
 *
 * @param[out] destination Eight bytes to write.
 * @param[in]  value       Value to store.
 */
void lazy_put_le64(void *destination, uint64_t value)
{
	uint8_t *bytes = destination;

	lazy_put_le32(bytes, (uint32_t)value);
	lazy_put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

/**
 * @brief Fold one byte into the exFAT rotating 32-bit checksum.
 *
 * @param[in] checksum Current checksum.
 * @param[in] value    Byte to add.
 * @return The updated checksum.
 */
uint32_t lazy_checksum32(uint32_t checksum, uint8_t value)
{
	return ((checksum & 1U) != 0U ? 0x80000000U : 0U) + (checksum >> 1) + value;
}

/**
 * @brief Enter a block device callback, refusing new ones once stop began.
 *
 * @return True when the callback may run; the caller must call
 *         @c lazy_armed_leave() before returning.
 */
bool lazy_armed_enter(void)
{
	if (!__atomic_load_n(&lazy.armed, __ATOMIC_SEQ_CST)) {
		return false;
	}
	(void)__atomic_add_fetch(&lazy.callbacks, 1U, __ATOMIC_SEQ_CST);
	return true;
}

/**
 * @brief Leave a block device callback entered with @c lazy_armed_enter().
 */
void lazy_armed_leave(void)
{
	(void)__atomic_sub_fetch(&lazy.callbacks, 1U, __ATOMIC_SEQ_CST);
}

/**
 * @brief Report whether a directory count or load should stop.
 *
 * @return True when cancellation was requested.
 */
bool lazy_cancel_requested(void)
{
	return __atomic_load_n(&lazy.cancel, __ATOMIC_SEQ_CST);
}

/**
 * @brief Report directory indexing progress to the installed callback.
 *
 * @details Called under the view lock. The callback must be short and must not
 * call back into the proxy except to request cancellation.
 *
 * @param[in] entries Number of entries processed so far.
 * @param[in] path    Source path of the directory being indexed.
 */
void lazy_progress_report(uint32_t entries, const char *path)
{
	void (*callback)(void *context, uint32_t entries, const char *path) =
		lazy.progress_callback;

	if (callback != NULL) {
		callback(lazy.progress_context, entries, path);
	}
}

/**
 * @brief Restore the configured runtime limits.
 */
void lazy_limits_default(void)
{
	lazy.cache_entries_max = CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES;
	lazy.cache_bytes_max = CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES;
	lazy.dir_max_entries = CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES;
}
