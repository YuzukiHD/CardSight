/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file initcall.c
 * @brief Runs the init calls a driver or subsystem registered in the .initcall section.
 *
 * Init calls: run every entry of a level, lowest priority number first.
 */

#include <init.h>

/** @brief First entry of the linker-assembled .initcall section. */
extern const struct initcall __initcall_start[];
/** @brief One past the last entry of the .initcall section. */
extern const struct initcall __initcall_end[];

/**
 * @brief Run every init call of one level, ordered by priority.
 *
 * Levels 0 and 1 run on the boot stack before the scheduler; levels 2 and 3 run from the boot
 * task. Because the linker section keeps registration order, the function sorts indirectly by
 * scanning the whole section once per priority value from 0 to 255, so lower priority numbers
 * always run first within a level. The call is ignored and the return values are dropped; the
 * number of entries is small enough that the repeated scans cost nothing noticeable.
 *
 * @param[in] level Init level to run.
 */
void initcalls_run(uint32_t level)
{
	for (uint32_t priority = 0U; priority < 256U; priority++) {
		for (const struct initcall *call = __initcall_start; call < __initcall_end; call++) {
			if (call->level == level && call->priority == priority) {
				(void)call->function();
			}
		}
	}
}
