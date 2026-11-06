/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host stand-in for the bits of FreeRTOS the UI uses. */
#ifndef CARDSIGHT_HOST_FREERTOS_H
#define CARDSIGHT_HOST_FREERTOS_H

#include <stdlib.h>

static inline void *pvPortMalloc(size_t size)
{
	return malloc(size);
}

static inline void vPortFree(void *pointer)
{
	free(pointer);
}

#endif
