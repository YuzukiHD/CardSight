/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MMIO accessors. The value comes first, as everywhere in this tree.
 */

#ifndef CARDSIGHT_IO_H
#define CARDSIGHT_IO_H

#include <stdint.h>

static inline uint8_t readb(uintptr_t address)
{
	return *(volatile uint8_t *)address;
}

static inline void writeb(uint8_t value, uintptr_t address)
{
	*(volatile uint8_t *)address = value;
}

static inline uint32_t readl(uintptr_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void writel(uint32_t value, uintptr_t address)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile("fence o, o" ::: "memory");
}

static inline void setbits32(uintptr_t address, uint32_t bits)
{
	writel(readl(address) | bits, address);
}

static inline void clrbits32(uintptr_t address, uint32_t bits)
{
	writel(readl(address) & ~bits, address);
}

static inline void clrsetbits32(uintptr_t address, uint32_t clear, uint32_t set)
{
	writel((readl(address) & ~clear) | set, address);
}

#endif /* CARDSIGHT_IO_H */
