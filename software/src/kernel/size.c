/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file size.c
 * @brief Decimal capacity formatting, shared by the drivers, the shell and the application.
 */

#include <stdio.h>

#include <size.h>

/**
 * @brief Format a byte count with a decimal unit suffix.
 *
 * Picks the largest unit (TB, GB, MB, kB, B) the value reaches, rounds to the nearest whole
 * unit of 1000 rather than 1024, and prints the result into @p buffer. A NULL buffer or a zero
 * size leaves the output untouched.
 *
 * @param[in]  bytes  Capacity in bytes.
 * @param[out] buffer Destination buffer.
 * @param[in]  size   Size of @p buffer in bytes.
 */
void format_capacity(uint64_t bytes, char *buffer, size_t size)
{
	if (buffer == NULL || size == 0U) {
		return;
	}
	if (bytes >= 1000000000000ULL) {
		snprintf(buffer, size, "%llu TB",
			(unsigned long long)((bytes + 500000000000ULL) / 1000000000000ULL));
	} else if (bytes >= 1000000000ULL) {
		snprintf(buffer, size, "%llu GB",
			(unsigned long long)((bytes + 500000000ULL) / 1000000000ULL));
	} else if (bytes >= 1000000ULL) {
		snprintf(buffer, size, "%llu MB",
			(unsigned long long)((bytes + 500000ULL) / 1000000ULL));
	} else if (bytes >= 1000ULL) {
		snprintf(buffer, size, "%llu kB", (unsigned long long)((bytes + 500ULL) / 1000ULL));
	} else {
		snprintf(buffer, size, "%llu B", (unsigned long long)bytes);
	}
}
