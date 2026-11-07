/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file io.h
 * @brief MMIO accessors.
 *
 * The value comes first, as everywhere in this tree.
 */

#ifndef CARDSIGHT_IO_H
#define CARDSIGHT_IO_H

#include <stdint.h>

/**
 * @brief Read one byte from a memory-mapped register.
 *
 * @param[in] address Register address.
 * @return The byte at @p address.
 */
static inline uint8_t readb(uintptr_t address)
{
	return *(volatile uint8_t *)address;
}

/**
 * @brief Write one byte to a memory-mapped register.
 *
 * @param[in] value   Byte to store.
 * @param[in] address Register address.
 */
static inline void writeb(uint8_t value, uintptr_t address)
{
	*(volatile uint8_t *)address = value;
}

/**
 * @brief Read one 32-bit word from a memory-mapped register.
 *
 * @param[in] address Register address.
 * @return The word at @p address.
 */
static inline uint32_t readl(uintptr_t address)
{
	return *(volatile uint32_t *)address;
}

/**
 * @brief Write one 32-bit word to a memory-mapped register.
 *
 * @details The store is followed by an I/O fence, so it reaches the device before any later
 * access proceeds.
 *
 * @param[in] value   Word to store.
 * @param[in] address Register address.
 */
static inline void writel(uint32_t value, uintptr_t address)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile("fence o, o" ::: "memory");
}

/**
 * @brief Set bits in a 32-bit register, leaving the others unchanged.
 *
 * @param[in] address Register address.
 * @param[in] bits    Bits to set.
 */
static inline void setbits32(uintptr_t address, uint32_t bits)
{
	writel(readl(address) | bits, address);
}

/**
 * @brief Clear bits in a 32-bit register, leaving the others unchanged.
 *
 * @param[in] address Register address.
 * @param[in] bits    Bits to clear.
 */
static inline void clrbits32(uintptr_t address, uint32_t bits)
{
	writel(readl(address) & ~bits, address);
}

/**
 * @brief Clear some bits and set others in one read-modify-write.
 *
 * @param[in] address Register address.
 * @param[in] clear   Bits to clear.
 * @param[in] set     Bits to set after the clear.
 */
static inline void clrsetbits32(uintptr_t address, uint32_t clear, uint32_t set)
{
	writel((readl(address) & ~clear) | set, address);
}

#endif /* CARDSIGHT_IO_H */
