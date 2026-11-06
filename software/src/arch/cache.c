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

#ifdef CONFIG_SHELL
#include <hal/io.h>
#include <hal/kernel.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_SMALL (8U * 1024U)
#define CACHE_LARGE (256U * 1024U)
#define CACHE_BYTES (8U * 1024U * 1024U)
/* The loader's SRAM at 0x00100000 holds the USB transfer buffer in its first 64 KiB;
 * 0x00110000 is the half the running image can use. */
#define SRAM_PROBE	0x00110000U
#define SRAM_SIZE	(32U * 1024U)

static uint8_t probe_small[CACHE_SMALL] __attribute__((aligned(64)));
static uint8_t probe_large[CACHE_LARGE] __attribute__((aligned(64)));
static volatile uint32_t probe_sink;

static uint32_t probe_read(const uint8_t *buffer, size_t bytes)
{
	const volatile uint32_t *words = (const volatile uint32_t *)buffer;
	uint32_t sum = 0U;

	for (size_t index = 0U; index < bytes / 4U; index++) {
		sum += words[index];
	}
	return sum;
}

static void probe_write(uint8_t *buffer, size_t bytes, uint32_t value)
{
	volatile uint32_t *words = (volatile uint32_t *)buffer;

	for (size_t index = 0U; index < bytes / 4U; index++) {
		words[index] = value;
	}
}

/* The state of the two caches, then the same amount of traffic over a buffer that fits the
 * data cache and one that does not: a working cache makes the first far faster. */
/* The PSRAM controller fills a line of its own per request (SQPI cache register, bits 1:0).
 * Match it to the 64-byte L1 line and time the streaming read for every setting. */
#define SQPI_CACHE_REG (0x03043000UL + 0x008U)

static void cache_read_line(uint32_t setting)
{
	uint32_t value = readl(SQPI_CACHE_REG);
	uint64_t started;
	uint64_t elapsed;

	writel((value & ~3U) | (setting & 3U), SQPI_CACHE_REG);
	started = ktime_get_us();
	probe_sink = probe_read(probe_large, CACHE_LARGE);
	elapsed = ktime_get_us() - started;
	printf("cache: read line %u (reg %08lx): 256 KiB in %llu us (%llu KiB/s)\n", (uint32_t)setting,
		(unsigned long)readl(SQPI_CACHE_REG), (unsigned long long)elapsed,
		(unsigned long long)((uint64_t)CACHE_LARGE * 1000000U / 1024U / (elapsed == 0U ? 1U : elapsed)));
	writel(value, SQPI_CACHE_REG);
}

static int command_cache(int argc, char **argv)
{
	uint32_t mxstatus;
	uint32_t mhcr;
	uint32_t mcor;
	uint32_t mhint;
	uint64_t started;
	uint64_t elapsed;
	uint32_t passes;

	(void)argv;
	if (argc == 2 && strcmp(argv[1], "readline") == 0) {
		for (uint32_t setting = 0U; setting < 4U; setting++) {
			cache_read_line(setting);
		}
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "mhint") == 0) {
		uint32_t saved;
		uint32_t setting = (uint32_t)strtoul(argv[2], NULL, 16);
		uint64_t started;
		uint64_t elapsed;

		__asm__ volatile("csrr %0, 0x7c5" : "=r"(saved));
		__asm__ volatile("csrw 0x7c5, %0" ::"r"(setting));
		started = ktime_get_us();
		probe_sink = probe_read(probe_large, CACHE_LARGE);
		elapsed = ktime_get_us() - started;
		__asm__ volatile("csrw 0x7c5, %0" ::"r"(saved));
		printf("cache: mhint %08lx: 256 KiB in %llu us (%llu KiB/s)\n",
			(unsigned long)setting, (unsigned long long)elapsed,
			(unsigned long long)((uint64_t)CACHE_LARGE * 1000000U / 1024U /
				(elapsed == 0U ? 1U : elapsed)));
		return 0;
	}
	__asm__ volatile("csrr %0, 0x7c0" : "=r"(mxstatus));
	__asm__ volatile("csrr %0, 0x7c1" : "=r"(mhcr));
	__asm__ volatile("csrr %0, 0x7c2" : "=r"(mcor));
	__asm__ volatile("csrr %0, 0x7c5" : "=r"(mhint));
	printf("cache: mxstatus %08lx mhcr %08lx mcor %08lx mhint %08lx\n",
		(unsigned long)mxstatus, (unsigned long)mhcr, (unsigned long)mcor,
		(unsigned long)mhint);
	printf("cache: icache %s, dcache %s, %s, %s\n", (mhcr & 1U) != 0U ? "on" : "off",
		(mhcr & 2U) != 0U ? "on" : "off", (mhcr & 4U) != 0U ? "write-back" : "write-through",
		(mhcr & 8U) != 0U ? "write-allocate" : "no write-allocate");

	passes = CACHE_BYTES / CACHE_SMALL;
	(void)probe_read(probe_small, CACHE_SMALL);
	started = ktime_get_us();
	for (uint32_t pass = 0U; pass < passes; pass++) {
		probe_sink = probe_read(probe_small, CACHE_SMALL);
	}
	elapsed = ktime_get_us() - started;
	printf("cache: read %u KiB x%u: %llu KiB/s\n", (uint32_t)(CACHE_SMALL / 1024U),
		(uint32_t)passes, (unsigned long long)((uint64_t)CACHE_BYTES * 1000000U / 1024U /
			(elapsed == 0U ? 1U : elapsed)));

	passes = CACHE_BYTES / CACHE_LARGE;
	started = ktime_get_us();
	for (uint32_t pass = 0U; pass < passes; pass++) {
		probe_sink = probe_read(probe_large, CACHE_LARGE);
	}
	elapsed = ktime_get_us() - started;
	printf("cache: read %u KiB x%u: %llu KiB/s\n", (uint32_t)(CACHE_LARGE / 1024U),
		(uint32_t)passes, (unsigned long long)((uint64_t)CACHE_BYTES * 1000000U / 1024U /
			(elapsed == 0U ? 1U : elapsed)));

	passes = CACHE_BYTES / CACHE_SMALL;
	started = ktime_get_us();
	for (uint32_t pass = 0U; pass < passes; pass++) {
		probe_write(probe_small, CACHE_SMALL, pass);
	}
	elapsed = ktime_get_us() - started;
	printf("cache: write %u KiB x%u: %llu KiB/s\n", (uint32_t)(CACHE_SMALL / 1024U),
		(uint32_t)passes, (unsigned long long)((uint64_t)CACHE_BYTES * 1000000U / 1024U /
			(elapsed == 0U ? 1U : elapsed)));

	passes = CACHE_BYTES / CACHE_LARGE;
	started = ktime_get_us();
	for (uint32_t pass = 0U; pass < passes; pass++) {
		probe_write(probe_large, CACHE_LARGE, pass);
	}
	elapsed = ktime_get_us() - started;
	printf("cache: write %u KiB x%u: %llu KiB/s\n", (uint32_t)(CACHE_LARGE / 1024U),
		(uint32_t)passes, (unsigned long long)((uint64_t)CACHE_BYTES * 1000000U / 1024U /
			(elapsed == 0U ? 1U : elapsed)));

	passes = CACHE_BYTES / SRAM_SIZE;
	started = ktime_get_us();
	for (uint32_t pass = 0U; pass < passes; pass++) {
		probe_write((uint8_t *)SRAM_PROBE, SRAM_SIZE, pass);
	}
	elapsed = ktime_get_us() - started;
	printf("cache: sram write %u KiB x%u: %llu KiB/s\n", (uint32_t)(SRAM_SIZE / 1024U),
		(uint32_t)passes, (unsigned long long)((uint64_t)CACHE_BYTES * 1000000U / 1024U /
			(elapsed == 0U ? 1U : elapsed)));

	started = ktime_get_us();
	for (uint32_t pass = 0U; pass < passes; pass++) {
		probe_sink = probe_read((const uint8_t *)SRAM_PROBE, SRAM_SIZE);
	}
	elapsed = ktime_get_us() - started;
	printf("cache: sram read %u KiB x%u: %llu KiB/s\n", (uint32_t)(SRAM_SIZE / 1024U),
		(uint32_t)passes, (unsigned long long)((uint64_t)CACHE_BYTES * 1000000U / 1024U /
			(elapsed == 0U ? 1U : elapsed)));
	return 0;
}
SHELL_CMD(cache, "cache: the XuanTie cache registers and a bandwidth check", command_cache);
#endif
