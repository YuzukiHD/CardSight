/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file init.h
 * @brief Init calls: functions that run in order after the kernel is up.
 *
 * Levels 0 and 1 run on the boot stack before the scheduler starts; levels 2 and 3 run from the
 * boot task. Within a level a lower priority number runs first.
 */

#ifndef CARDSIGHT_INIT_H
#define CARDSIGHT_INIT_H

#include <stdint.h>

/**
 * @brief Signature of an init call.
 *
 * @return 0 on success, or a negative errno; the runner ignores the value.
 */
typedef int (*initcall_fn)(void);

/**
 * @brief One entry of the .initcall linker section.
 */
struct initcall {
	initcall_fn function; /**< Function to run. */
	unsigned char level; /**< Init level the call belongs to. */
	unsigned char priority; /**< Order within the level; lower runs first. */
};

/**
 * @brief Register a function as an init call.
 *
 * @details Places a static struct initcall in the .initcall section, which initcalls_run()
 * walks. The symbol is named after @p _fn and kept even when nothing references it.
 *
 * @param _level    Init level (0 to 3).
 * @param _priority Order within the level; lower runs first.
 * @param _fn       Function with the initcall_fn signature.
 */
#define INITCALL(_level, _priority, _fn)                                                          \
	static const struct initcall __initcall_##_fn __attribute__((used, section(".initcall"))) = { \
		.function = (_fn),                                                                        \
		.level = (_level),                                                                        \
		.priority = (_priority),                                                                  \
	}

/**
 * @brief Run every init call of one level.
 *
 * @details Walks priorities from 0 to 255 and, for each, the whole .initcall section, calling
 * the entries whose level and priority match. Return values are ignored.
 *
 * @param[in] level Init level to run.
 */
void initcalls_run(uint32_t level);

#endif /* CARDSIGHT_INIT_H */
