/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file console.c
 * @brief The console over UART0: polled output, interrupt driven input.
 *
 * Wraps the sunxi UART driver for the stream that the kernel log, the shell and the C
 * library's stdout share.
 */

#include <init.h>

#include <console.h>

#include "drivers/drivers.h"

/**
 * @brief Write a buffer to the console, turning each LF into CR LF.
 *
 * Output is polled through the UART driver, so it works before the scheduler and from
 * interrupt context.
 *
 * @param[in] data   Bytes to write.
 * @param[in] length Number of bytes in @p data.
 */
void console_write(const char *data, size_t length)
{
	for (size_t index = 0U; index < length; index++) {
		if (data[index] == '\n') {
			sunxi_uart_putc('\r');
		}
		sunxi_uart_putc(data[index]);
	}
}

/**
 * @brief Read one byte from the console.
 *
 * @param[in] timeout_ms Maximum wait in milliseconds; a negative value waits without a
 *                       practical limit.
 * @retval byte  Value of the received byte.
 * @retval -EAGAIN Nothing arrived within the timeout.
 * @retval -ENODEV Reception was never started.
 */
int console_read(int timeout_ms)
{
	return sunxi_uart_getc(timeout_ms);
}

/**
 * @brief Switch the UART receive path to interrupts.
 *
 * Needs the interrupt controller, so it runs as an init call rather than from the boot path.
 *
 * @retval 0       Reception is armed.
 * @retval -ENODEV The UART or its IRQ source is not configured.
 * @retval -ENOMEM The receive queue could not be allocated.
 * @retval -EINVAL The interrupt could not be connected.
 */
int console_start_receive(void)
{
	return sunxi_uart_start_rx();
}
/** @brief Enable console receive interrupts at init level 2, priority 10. */
INITCALL(2, 10, console_start_receive);
