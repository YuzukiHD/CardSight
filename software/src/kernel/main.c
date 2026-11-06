/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Boot: bring the console up first (it is polled), then the interrupt controller and the timer,
 * run the early init calls and start the scheduler. The boot task runs the late init calls and
 * hands over to the application's app_main().
 */

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


extern char __heap_start[];
extern char __heap_end[];
extern char __image_start[];
extern char __image_end[];

/* The application provides this; without one the image is a console with a shell. */
void app_main(void) __attribute__((weak));

void app_main(void)
{
	pr_info("no application linked\n");
}

static void boot_task(void *argument)
{
	(void)argument;
	initcalls_run(2);
	initcalls_run(3);
	app_main();
	vTaskDelete(NULL);
}

void main(void)
{
	(void)sunxi_uart_init(&sunxi_uart0);
	setvbuf(stdout, NULL, _IONBF, 0);
	cs_clint_init();
	cs_plic_init();

	pr_info("CardSight on C907, %lu Hz timer, image %u KiB, heap %u KiB\n",
		(unsigned long)SUNXI_TIMEBASE_HZ, (unsigned)((__image_end - __image_start) / 1024U),
		(unsigned)((__heap_end - __heap_start) / 1024U));
	pr_info("PSRAM %u MiB at 0x%08lx\n", (unsigned)(SUNXI_PSRAM_SIZE / (1024U * 1024U)),
		(unsigned long)SUNXI_PSRAM_BASE);

	initcalls_run(0);
	initcalls_run(1);

	if (xTaskCreate(boot_task, "boot", 4096U, NULL, 6U, NULL) != pdPASS) {
		cs_panic("cannot create the boot task");
	}
	/* From here the FreeRTOS trap handler takes over; the machine timer and external
	 * interrupts must be enabled by us because the CLINT addresses in the port are 0. */
	extern void freertos_risc_v_trap_handler(void);
	__asm__ volatile("csrw mtvec, %0" ::"r"(freertos_risc_v_trap_handler));
	__asm__ volatile("csrs mie, %0" ::"r"(0x880));
	vTaskStartScheduler();
	cs_panic("the scheduler returned");
}
