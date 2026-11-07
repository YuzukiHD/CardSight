/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file log.c
 * @brief Kernel log messages on the console, with a timestamp and a coloured level mark.
 *
 * Format and appearance match the SyterKit log messages around them:
 * [seconds.microseconds][level] module: message, with the level in colour.
 */

#include <stdarg.h>
#include <stdio.h>

#include <console.h>
#include <hal/ktime.h>
#include <log.h>

/* The level letters and their colours, indexed by level as log.h numbers them. */
/**
 * @brief Printed mark for each log level, carrying its ANSI colour.
 *
 * Index 0 is the fallback used for an out-of-range level.
 */
static const char *const level_marks[] = {
	[0] = "?",
	[LOG_LEVEL_ERROR] = "\033[31mE\033[0m",
	[LOG_LEVEL_WARN] = "\033[33mW\033[0m",
	[LOG_LEVEL_INFO] = "\033[36mI\033[0m",
	[LOG_LEVEL_DEBUG] = "\033[32mD\033[0m",
};

/**
 * @brief Print one log line on the console.
 *
 * Returns early when @p level is above CONFIG_LOG_LEVEL, so debug output costs one comparison
 * in release configurations. The message is formatted into a fixed 192-byte buffer and the
 * prefix "[seconds.microseconds][mark] module: " into a 64-byte one; both are then written with
 * console_write(), clamping the length to the buffer when snprintf truncated. There is no
 * locking, so concurrent callers can interleave their prefix and message.
 *
 * @param[in] level  Log level, one of the LOG_LEVEL_* values.
 * @param[in] module Module name printed before the message.
 * @param[in] format printf-style format string.
 * @param[in] ...    Arguments for @p format.
 */
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
