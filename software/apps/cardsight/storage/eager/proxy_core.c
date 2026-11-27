// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy_core.c
 * @brief The view instance, its counters and the kernel heap allocators.
 *
 * @details Part of the eager exFAT proxy engine. The shared state and helpers
 * live in @c proxy_types.h; the cross-module interfaces in @c proxy_private.h.
 */
#include "proxy_private.h"

/* The view and its counters, the only instances; every other module reaches them through the header. */
/** @brief The one view instance. */
struct storage_proxy proxy;
/** @brief Counters and timings of @c proxy. */
struct proxy_statistics proxy_stats;

/* The C library heap is small, so everything the view allocates comes from the kernel heap. */
#ifdef STORAGE_PROXY_HOST_TEST
/**
 * @brief Allocate memory for the view, host-test variant.
 *
 * @param[in] size Bytes to allocate.
 * @return The allocation, or NULL.
 */
void *proxy_alloc(size_t size)
{
	return malloc(size);
}

/**
 * @brief Release memory obtained from @c proxy_alloc(), host-test variant.
 *
 * @param[in] pointer Allocation to free.
 */
void proxy_free(void *pointer)
{
	free(pointer);
}
#else
/**
 * @brief Allocate memory for the view from the kernel heap.
 *
 * @param[in] size Bytes to allocate.
 * @return The allocation, or NULL.
 */
void *proxy_alloc(size_t size)
{
	return pvPortMalloc(size);
}

/**
 * @brief Release memory obtained from @c proxy_alloc().
 *
 * @param[in] pointer Allocation to free.
 */
void proxy_free(void *pointer)
{
	vPortFree(pointer);
}
#endif

/**
 * @brief Allocate zeroed memory for the view.
 *
 * @details The multiplication is checked for overflow before the allocation,
 * because the size comes from configuration values.
 *
 * @param[in] count Number of elements.
 * @param[in] size  Size of one element.
 * @return The zeroed allocation, or NULL.
 */
void *proxy_calloc(size_t count, size_t size)
{
	void *pointer;

	if (count != 0U && size > SIZE_MAX / count) {
		return NULL;
	}
	pointer = proxy_alloc(count * size);
	if (pointer != NULL) {
		memset(pointer, 0, count * size);
	}
	return pointer;
}




/**
 * @brief Initialise the view's instance and its lock.
 *
 * @return 0 on success, or -ENOMEM when the lock cannot be created.
 */
int storage_proxy_init(void)
{
	memset(&proxy, 0, sizeof(proxy));
	proxy.cached_fd = -1;
	proxy.cached_node = PROXY_INVALID_NODE;
	mutex_init(&proxy.lock);
	return proxy.lock.handle == NULL ? -ENOMEM : 0;
}

/** @brief Register the view initialiser with the init call table. */
INITCALL(3, 20, storage_proxy_init);
