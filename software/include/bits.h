/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file bits.h
 * @brief Small helpers shared by the whole tree: array sizes, value bounds, and address
 * arithmetic.
 */

#ifndef CARDSIGHT_BITS_H
#define CARDSIGHT_BITS_H

#include <stddef.h>

/** @brief Number of elements in a fixed-size array. */
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
/**
 * @brief Smaller of @p a and @p b.
 *
 * @note The selected argument is evaluated twice; do not pass side-effecting expressions.
 */
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
/**
 * @brief Larger of @p a and @p b.
 *
 * @note The selected argument is evaluated twice; do not pass side-effecting expressions.
 */
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
/** @brief @p value limited to the range from @p low to @p high. */
#define CLAMP(value, low, high) ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)))
/** @brief Value with bit @p n set. */
#define BIT(n) (1UL << (n))
/**
 * @brief @p value rounded up to the next multiple of @p alignment.
 *
 * @note @p alignment must be a power of two.
 */
#define ALIGN_UP(value, alignment) (((value) + (alignment) - 1U) & ~((alignment) - 1U))
/**
 * @brief Address of the structure that contains @p pointer as its @p member.
 *
 * @param pointer Address of the member.
 * @param type    Type of the enclosing structure.
 * @param member  Name of the member within that structure.
 */
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))

#endif /* CARDSIGHT_BITS_H */
