/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Kernel log messages: printf-style output at a level, prefixed with the module that prints.
 * LOG_MODULE defaults to the file name; a file may define it before including this header.
 */

#ifndef CARDSIGHT_LOG_H
#define CARDSIGHT_LOG_H

#include <stdio.h>

#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

#ifndef CONFIG_LOG_LEVEL
#define CONFIG_LOG_LEVEL LOG_LEVEL_INFO
#endif

#ifndef LOG_MODULE
#define LOG_MODULE "kernel"
#endif

void log_print(int level, const char *module, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

#define pr_err(format, ...) log_print(LOG_LEVEL_ERROR, LOG_MODULE, format, ##__VA_ARGS__)
#define pr_warn(format, ...) log_print(LOG_LEVEL_WARN, LOG_MODULE, format, ##__VA_ARGS__)
#define pr_info(format, ...) log_print(LOG_LEVEL_INFO, LOG_MODULE, format, ##__VA_ARGS__)
#define pr_debug(format, ...) log_print(LOG_LEVEL_DEBUG, LOG_MODULE, format, ##__VA_ARGS__)

#endif /* CARDSIGHT_LOG_H */
