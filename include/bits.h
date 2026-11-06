/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Small helpers shared by the whole tree: the sizes of arrays, the value bounds and address
 * arithmetic.
 */

#ifndef CARDSIGHT_BITS_H
#define CARDSIGHT_BITS_H

#include <stddef.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(value, low, high) ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)))
#define BIT(n) (1UL << (n))
#define ALIGN_UP(value, alignment) (((value) + (alignment) - 1U) & ~((alignment) - 1U))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))

#endif /* CARDSIGHT_BITS_H */
