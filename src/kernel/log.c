/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Kernel log messages on the console, in the same shape as the SyterKit log messages around
 * them: [seconds.microseconds][level] module: message, with the level in colour.
 */

#include <stdarg.h>
#include <stdio.h>

#include <console.h>
#include <hal/ktime.h>
#include <log.h>

/* The level letters and their colours, indexed by level as log.h numbers them. */
static const char *const level_marks[] = {
	[0] = "?",
	[LOG_LEVEL_ERROR] = "\033[31mE\033[0m",
	[LOG_LEVEL_WARN] = "\033[33mW\033[0m",
	[LOG_LEVEL_INFO] = "\033[36mI\033[0m",
	[LOG_LEVEL_DEBUG] = "\033[32mD\033[0m",
};

void log_print(int level, const char *module, const char *format, ...)
{
	char message[192];
	char prefix[64];
	uint64_t now_us;
	int length;
	int prefix_length;
	va_list arguments;

	if (level > CONFIG_LOG_LEVEL) {
		return;
	}
	va_start(arguments, format);
	length = vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	if (length < 0) {
		return;
	}
	now_us = ktime_get_us();
	prefix_length = snprintf(prefix, sizeof(prefix), "[%5lu.%06lu][%s] %s: ",
		(unsigned long)(now_us / 1000000U), (unsigned long)(now_us % 1000000U),
		level >= 0 && level <= LOG_LEVEL_DEBUG ? level_marks[level] : level_marks[0], module);
	if (prefix_length > 0) {
		console_write(prefix, (size_t)prefix_length < sizeof(prefix) ? (size_t)prefix_length :
										sizeof(prefix) - 1U);
	}
	console_write(message, (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1U);
}
