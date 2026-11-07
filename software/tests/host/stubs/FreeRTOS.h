/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file FreeRTOS.h
 * @brief Host stand-in for the bits of FreeRTOS the UI uses.
 *
 * The host UI simulation does not run the FreeRTOS heap, so the two allocator
 * entry points simply forward to the C library.
 */
#ifndef CARDSIGHT_HOST_FREERTOS_H
#define CARDSIGHT_HOST_FREERTOS_H

#include <stdlib.h>

/**
 * @brief Allocate memory from the emulated FreeRTOS heap.
 *
 * @param[in] size Number of bytes to allocate.
 * @return Pointer to the block, or NULL when the allocation fails.
 */
static inline void *pvPortMalloc(size_t size)
{
	return malloc(size);
}

/**
 * @brief Release memory previously returned by pvPortMalloc().
 *
 * @param[in] pointer Block to free; NULL is accepted.
 */
static inline void vPortFree(void *pointer)
{
	free(pointer);
}

#endif
