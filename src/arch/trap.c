/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fatal traps: before the scheduler the trap vector points at early_trap (start.S), which hands
 * the cause here; after it, FreeRTOS' trap handler calls the application exception handler for
 * anything it does not handle itself.
 */

#include <stdint.h>

#include "arch/arch.h"
#include "drivers/drivers.h"

static void put_string(const char *text)
{
	while (*text != '\0') {
		sunxi_uart_putc(*text++);
	}
}

static void put_hex(uint32_t value)
{
	static const char digits[] = "0123456789abcdef";

	for (int shift = 28; shift >= 0; shift -= 4) {
		sunxi_uart_putc(digits[(value >> shift) & 0xfU]);
	}
}

static void report(const char *what, uint32_t cause, uint32_t epc)
{
	put_string("\r\n*** ");
	put_string(what);
	put_string(" mcause=0x");
	put_hex(cause);
	put_string(" mepc=0x");
	put_hex(epc);
	put_string(" ***\r\n");
}

void cs_panic(const char *message)
{
	put_string("\r\n*** panic: ");
	put_string(message);
	put_string(" ***\r\n");
	__asm__ volatile("csrw mie, zero" ::: "memory");
	for (;;) {
		__asm__ volatile("wfi");
	}
}

void early_trap_report(uint32_t cause, uint32_t epc);

void early_trap_report(uint32_t cause, uint32_t epc)
{
	report("fatal trap", cause, epc);
	__asm__ volatile("csrw mie, zero" ::: "memory");
	for (;;) {
		__asm__ volatile("wfi");
	}
}

/* Called by the FreeRTOS RISC-V port with the cause in a0. */
void freertos_risc_v_application_exception_handler(uintptr_t cause);

void freertos_risc_v_application_exception_handler(uintptr_t cause)
{
	uint32_t epc;

	__asm__ volatile("csrr %0, mepc" : "=r"(epc));
	report("exception", (uint32_t)cause, epc);
	__asm__ volatile("csrw mie, zero" ::: "memory");
	for (;;) {
		__asm__ volatile("wfi");
	}
}
