/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file cache.c
 * @brief XuanTie C907 cache maintenance (T-Head CMO extension).
 *
 * This file alone is compiled with xtheadcmo, and the instructions take physical addresses,
 * which is what the DMA engines use. With CONFIG_SHELL the file also defines the "cache"
 * command, which dumps the cache control registers and measures memory throughput.
 */

#include <stdint.h>

#include <hal/cache.h>

/** @brief Mask selecting the offset of a byte within one cache line. */
#define LINE_MASK (CACHE_LINE_BYTES - 1U)

/**
 * @brief Define a cache maintenance function that walks a memory range line by line.
 *
 * The generated function first fences memory and the instruction stream, then applies the
 * given CMO instruction to every cache line covering the range, starting from the line that
 * contains @c address and iterating while the line start is below @c address + @c length.
 * An empty range returns without touching anything.
 *
 * @param name        Name of the function to define.
 * @param instruction CMO instruction applied to each line (e.g. "th.dcache.cpa").
 */
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

/** @brief Write back every dirty line of a range so that memory sees the data. */
RANGE_OP(dcache_clean_range, "th.dcache.cpa")
/** @brief Drop the cached copies of a range so that the next read comes from memory. */
RANGE_OP(dcache_invalidate_range, "th.dcache.ipa")

#ifdef CONFIG_SHELL
#include <hal/io.h>
#include <hal/kernel.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Buffer size that fits in the data cache, in bytes. */
#define CACHE_SMALL (8U * 1024U)
/** @brief Buffer size larger than the data cache, in bytes. */
#define CACHE_LARGE (256U * 1024U)
/** @brief Total traffic the bandwidth measurements run over, in bytes. */
#define CACHE_BYTES (8U * 1024U * 1024U)
/* The loader's SRAM at 0x00100000 holds the USB transfer buffer in its first 64 KiB;
 * 0x00110000 is the half the running image can use. */
/** @brief Start address of the free half of the loader's SRAM. */
#define SRAM_PROBE	0x00110000U
/** @brief Size of the probed SRAM window, in bytes. */
#define SRAM_SIZE	(32U * 1024U)

/** @brief Read buffer that fits in the data cache, line aligned. */
static uint8_t probe_small[CACHE_SMALL] __attribute__((aligned(64)));
/** @brief Read buffer larger than the data cache, line aligned. */
static uint8_t probe_large[CACHE_LARGE] __attribute__((aligned(64)));
/** @brief Accumulator that keeps the probe reads from being optimised away. */
static volatile uint32_t probe_sink;

/**
 * @brief Sum a buffer word by word through volatile accesses.
 *
 * @param[in] buffer Buffer to read.
 * @param[in] bytes  Number of bytes to read; a trailing partial word is ignored.
 * @return Sum of the 32-bit words.
 */
static uint32_t probe_read(const uint8_t *buffer, size_t bytes)
{
	const volatile uint32_t *words = (const volatile uint32_t *)buffer;
	uint32_t sum = 0U;

	for (size_t index = 0U; index < bytes / 4U; index++) {
		sum += words[index];
	}
	return sum;
}

/**
 * @brief Fill a buffer word by word through volatile accesses.
 *
 * The volatile stores make sure the write traffic really reaches the memory hierarchy.
 *
 * @param[out] buffer Buffer to fill.
 * @param[in]  bytes  Number of bytes to write; a trailing partial word is ignored.
 * @param[in]  value  Word value written to every location.
 */
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
/** @brief PSRAM (SQPI) controller cache-line configuration register. */
#define SQPI_CACHE_REG (0x03043000UL + 0x008U)

/**
 * @brief Measure the streaming read speed for one PSRAM line-fill setting.
 *
 * Saves the register, programs the low two bits with @p setting, reads the large buffer once
 * while timing it with ktime and prints the rate, then restores the original register value.
 *
 * @param[in] setting Value 0 to 3 written to the SQPI cache-line field.
 */
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

/**
 * @brief Shell command for the cache registers and memory bandwidth.
 *
 * With no argument it reads mxstatus, mhcr, mcor and mhint, prints the control-register state
 * and then runs the bandwidth benchmark: the same total traffic (CACHE_BYTES) over a buffer
 * that fits the data cache and one that does not, for reads and for writes, followed by a
 * write and a read pass over the free half of the loader's SRAM. A working cache makes the
 * small-buffer runs far faster than the large-buffer ones.
 *
 * @p argv selects three variants instead: "readline" sweeps the four SQPI cache-line settings,
 * "psram <divider>" reprograms the SQPI clock divider before reading, and
 * "mhint <hex>" temporarily writes the mhint CSR while reading.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector; @c argv[0] is the command name.
 * @return 0 when the measurements completed.
 */
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
	if (argc == 3 && strcmp(argv[1], "psram") == 0) {
		/* SQPI module clock: CCU + 0x850, divider-1 in bits 3:0, source in bits 25:24,
		 * gate in bit 31. The parent is the 400 MHz peripheral PLL. */
		uintptr_t clock_reg = 0x02001000UL + 0x850U;
		uint32_t divider = (uint32_t)strtoul(argv[2], NULL, 10);
		uint32_t value = readl(clock_reg);
		uint64_t started;
		uint64_t elapsed;

		writel(value & ~(1U << 31), clock_reg);
		writel((value & ~0xfU) | ((divider - 1U) & 0xfU), clock_reg);
		started = ktime_get_us();
		probe_sink = probe_read(probe_large, CACHE_LARGE);
		elapsed = ktime_get_us() - started;
		printf("cache: psram clock /%u (reg %08lx): 256 KiB in %llu us (%llu KiB/s)\n",
			(uint32_t)divider, (unsigned long)readl(clock_reg),
			(unsigned long long)elapsed,
			(unsigned long long)((uint64_t)CACHE_LARGE * 1000000U / 1024U /
				(elapsed == 0U ? 1U : elapsed)));
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
/** @brief Register the "cache" command. */
SHELL_CMD(cache, "cache: the XuanTie cache registers and a bandwidth check", command_cache);
#endif
