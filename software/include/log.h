/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file log.h
 * @brief Kernel log messages: printf-style output at a level, prefixed with the module that
 * prints.
 *
 * LOG_MODULE defaults to the file name; a file may define it before including this header.
 */

#ifndef CARDSIGHT_LOG_H
#define CARDSIGHT_LOG_H

#include <stdio.h>

/** @brief Error level: a failure the user should notice. */
#define LOG_LEVEL_ERROR 1
/** @brief Warning level: something unexpected that did not stop the work. */
#define LOG_LEVEL_WARN  2
/** @brief Informational level: normal progress. */
#define LOG_LEVEL_INFO  3
/** @brief Debug level: developer detail. */
#define LOG_LEVEL_DEBUG 4

/** @brief Highest level that is printed; defaults to LOG_LEVEL_INFO. */
#ifndef CONFIG_LOG_LEVEL
#define CONFIG_LOG_LEVEL LOG_LEVEL_INFO
#endif

/** @brief Module name in every message; defaults to "kernel". */
#ifndef LOG_MODULE
#define LOG_MODULE "kernel"
#endif

/**
 * @brief Print one formatted log message to the console.
 *
 * @details Messages above @c CONFIG_LOG_LEVEL are dropped. The text is formatted into a
 * fixed-size buffer, prefixed with the time since boot, the level letter, and the module name,
 * then written to the console.
 *
 * @param[in] level  One of the LOG_LEVEL_* values.
 * @param[in] module Module name shown in the prefix.
 * @param[in] format printf-style format string.
 * @param[in] ...    Arguments for @p format.
 */
void log_print(int level, const char *module, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

/** @brief Log an error message; see log_print(). */
#define pr_err(format, ...) log_print(LOG_LEVEL_ERROR, LOG_MODULE, format, ##__VA_ARGS__)
/** @brief Log a warning message; see log_print(). */
#define pr_warn(format, ...) log_print(LOG_LEVEL_WARN, LOG_MODULE, format, ##__VA_ARGS__)
/** @brief Log an informational message; see log_print(). */
#define pr_info(format, ...) log_print(LOG_LEVEL_INFO, LOG_MODULE, format, ##__VA_ARGS__)
/** @brief Log a debug message; see log_print(). */
#define pr_debug(format, ...) log_print(LOG_LEVEL_DEBUG, LOG_MODULE, format, ##__VA_ARGS__)

#endif /* CARDSIGHT_LOG_H */
