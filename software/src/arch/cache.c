/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XuanTie C907 cache maintenance (T-Head CMO extension). This file alone is compiled with
 * xtheadcmo, and the instructions take physical addresses, which is what the DMA engines use.
 */

#include <stdint.h>

#include <hal/cache.h>

#define LINE_MASK (CACHE_LINE_BYTES - 1U)

#define RANGE_OP(name, instruction)                                                 \
	void name(const void *address, size_t length)                               \
	{                                                                           \
		uintptr_t position = (uintptr_t)address & ~(uintptr_t)LINE_MASK;    \
		uintptr_t end = (uintptr_t)address + length;                        \
										                            \
		if (length == 0U) {                                                 \
			return;                                                     \
		}                                                                   \
		__asm__ volatile("fence\n fence.i" ::: "memory");                   \
		for (; position < end; position += CACHE_LINE_BYTES) {              \
			__asm__ volatile(instruction " %0" ::"r"(position) : "memory"); \
		}                                                                   \
		__asm__ volatile("fence\n fence.i" ::: "memory");                   \
	}

RANGE_OP(dcache_clean_range, "th.dcache.cpa")
RANGE_OP(dcache_invalidate_range, "th.dcache.ipa")
