/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Decimal capacity formatting, shared by the drivers, the shell and the application. */

#include <stdio.h>

#include <size.h>

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
