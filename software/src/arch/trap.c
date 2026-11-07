/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file trap.c
 * @brief Fatal trap reporting on the polled UART, and the final park loop.
 *
 * Before the scheduler the trap vector points at early_trap (start.S), which hands the cause
 * here; after it, FreeRTOS' trap handler calls the application exception handler for anything
 * it does not handle itself. Every report goes straight to the UART driver because the console
 * and the kernel log may not be usable yet.
 */

#include <stdint.h>

#include "arch/arch.h"
#include "drivers/drivers.h"

/**
 * @brief Write a NUL-terminated string to the UART one byte at a time.
 *
 * @param[in] text String to write; must not be NULL.
 */
static void put_string(const char *text)
{
	while (*text != '\0') {
		sunxi_uart_putc(*text++);
	}
}

/**
 * @brief Write a 32-bit value as eight lower-case hexadecimal digits.
 *
 * Emits the digits from the most significant nibble down, without a "0x" prefix.
 *
 * @param[in] value Value to print.
 */
static void put_hex(uint32_t value)
{
	/** @brief Hexadecimal digit alphabet, indexed by nibble value. */
	static const char digits[] = "0123456789abcdef";

	for (int shift = 28; shift >= 0; shift -= 4) {
		sunxi_uart_putc(digits[(value >> shift) & 0xfU]);
	}
}

/**
 * @brief Print one "*** <what> mcause=0x... mepc=0x... ***" line.
 *
 * Wrapped in CR/LF pairs so the line starts on a fresh row whatever the console was doing.
 *
 * @param[in] what  Short description of the failure, e.g. "fatal trap".
 * @param[in] cause RISC-V mcause value.
 * @param[in] epc   RISC-V mepc value, the faulting instruction address.
 */
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

/**
 * @brief Print a panic message, disable machine interrupts and stop the hart.
 *
 * Used for failures that leave the system unusable, such as an exhausted kernel heap or a
 * stack overflow. It never returns; only a reset or a debugger gets the hart back.
 *
 * @param[in] message Text to print; must not be NULL.
 */
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

/**
 * @brief Report a fatal trap taken before the scheduler installed the FreeRTOS vector.
 *
 * Called from early_trap in start.S with the cause and the faulting address. Prints the
 * report on the polled UART, masks machine interrupts and parks the hart; nothing above the
 * trap can run safely at this point.
 *
 * @param[in] cause RISC-V mcause value.
 * @param[in] epc   RISC-V mepc value, the faulting instruction address.
 */
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

/**
 * @brief Report an exception the FreeRTOS port did not handle, then park the hart.
 *
 * Reads mepc for the faulting address, prints the cause and address, masks machine interrupts
 * and spins in a wait-for-interrupt loop. This is the last line of defence, so it makes no
 * attempt to recover or to return.
 *
 * @param[in] cause RISC-V mcause value, passed in a0 by the port.
 */
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
