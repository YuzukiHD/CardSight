/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Init calls: run every entry of a level, lowest priority number first. */

#include <init.h>

extern const struct initcall __initcall_start[];
extern const struct initcall __initcall_end[];

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
