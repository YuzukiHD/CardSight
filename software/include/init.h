/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Init calls: functions that run in order after the kernel is up.
 *
 * Levels 0 and 1 run on the boot stack before the scheduler starts; levels 2 and 3 run from the
 * boot task. Within a level a lower priority number runs first.
 */

#ifndef CARDSIGHT_INIT_H
#define CARDSIGHT_INIT_H

#include <stdint.h>

typedef int (*initcall_fn)(void);

struct initcall {
	initcall_fn function;
	unsigned char level;
	unsigned char priority;
};

#define INITCALL(_level, _priority, _fn)                                                          \
	static const struct initcall __initcall_##_fn __attribute__((used, section(".initcall"))) = { \
		.function = (_fn),                                                                        \
		.level = (_level),                                                                        \
		.priority = (_priority),                                                                  \
	}

void initcalls_run(uint32_t level);

#endif /* CARDSIGHT_INIT_H */
