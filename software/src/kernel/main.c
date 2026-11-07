/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file main.c
 * @brief Boot sequence: console, timer and interrupt controller, init calls, scheduler.
 *
 * Bring the console up first (it is polled), then the interrupt controller and the timer,
 * run the early init calls and start the scheduler. The boot task runs the late init calls
 * and hands over to the application's app_main().
 */

/** @brief Module name the boot messages are tagged with in the kernel log. */
#define LOG_MODULE "boot"

#include <init.h>
#include <stdint.h>
#include <stdio.h>

#include <console.h>
#include <log.h>

#include "FreeRTOS.h"
#include "task.h"

#include "arch/arch.h"
#include "board/cardsight_board.h"
#include "drivers/drivers.h"


/** @brief Start of the heap region, right after the image. */
extern char __heap_start[];
/** @brief End of the heap region. */
extern char __heap_end[];
/** @brief Start of the loaded image, used for size reporting. */
extern char __image_start[];
/** @brief End of the loaded image. */
extern char __image_end[];

/* The application provides this; without one the image is a console with a shell. */
void app_main(void) __attribute__((weak));

/**
 * @brief Default application entry point, linked when the image has no application.
 *
 * Logs that no application is linked, which leaves the console shell as the only interface.
 */
void app_main(void)
{
	pr_info("no application linked\n");
}

/**
 * @brief Task that finishes the boot once the scheduler is running.
 *
 * Runs the late init calls at levels 2 and 3, which may block, then calls app_main() and
 * deletes itself so its stack is reclaimed.
 *
 * @param[in] argument Unused task argument.
 */
static void boot_task(void *argument)
{
	(void)argument;
	initcalls_run(2);
	initcalls_run(3);
	app_main();
	vTaskDelete(NULL);
}

/**
 * @brief C entry point: bring up the machine and hand control to FreeRTOS.
 *
 * Order matters and is fixed by what each step depends on: the UART comes first because it is
 * polled and gives the only diagnostics; stdout is then made unbuffered so log lines appear
 * immediately; the machine timer records its boot value for ktime and the PLIC clears its
 * context. After the banner, init levels 0 and 1 run on the boot stack, because they may not
 * sleep. The boot task is created next, then mtvec is pointed at the FreeRTOS trap handler and
 * machine external and timer interrupts are enabled (mie = 0x880) by hand, since the port's
 * CLINT addresses are zero and its set-up does not cover them. vTaskStartScheduler() never
 * returns; reaching the panic after it means the scheduler failed.
 */
void main(void)
{
	(void)sunxi_uart_init(&sunxi_uart0);
	setvbuf(stdout, NULL, _IONBF, 0);
	cs_clint_init();
	cs_plic_init();

	pr_info("CardSight on C907, %lu Hz timer, image %u KiB, heap %u KiB\n",
		(unsigned long)SUNXI_TIMEBASE_HZ, (uint32_t)((__image_end - __image_start) / 1024U),
		(uint32_t)((__heap_end - __heap_start) / 1024U));
	pr_info("PSRAM %u MiB at 0x%08lx\n", (uint32_t)(SUNXI_PSRAM_SIZE / (1024U * 1024U)),
		(unsigned long)SUNXI_PSRAM_BASE);

	initcalls_run(0);
	initcalls_run(1);

	if (xTaskCreate(boot_task, "boot", 4096U, NULL, 6U, NULL) != pdPASS) {
		cs_panic("cannot create the boot task");
	}
	/* From here the FreeRTOS trap handler takes over; the machine timer and external
	 * interrupts must be enabled by us because the CLINT addresses in the port are 0. */
	/** @brief The port's assembly trap entry, installed in mtvec below. */
	extern void freertos_risc_v_trap_handler(void);
	__asm__ volatile("csrw mtvec, %0" ::"r"(freertos_risc_v_trap_handler));
	__asm__ volatile("csrs mie, %0" ::"r"(0x880));
	vTaskStartScheduler();
	cs_panic("the scheduler returned");
}
