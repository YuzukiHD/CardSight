/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The console over UART0: polled output, interrupt driven input. */

#include <init.h>

#include <console.h>

#include "drivers/drivers.h"

void console_write(const char *data, size_t length)
{
	for (size_t index = 0U; index < length; index++) {
		if (data[index] == '\n') {
			sunxi_uart_putc('\r');
		}
		sunxi_uart_putc(data[index]);
	}
}

int console_read(int timeout_ms)
{
	return sunxi_uart_getc(timeout_ms);
}

int console_start_receive(void)
{
	return sunxi_uart_start_rx();
}
INITCALL(2, 10, console_start_receive);
