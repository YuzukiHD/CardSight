/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What the drivers offer the rest of the platform. */

#ifndef CARDSIGHT_DRIVERS_H
#define CARDSIGHT_DRIVERS_H

#include <stdbool.h>
#include <stdint.h>
#include <board/cardsight_board.h>

/* clk/sunxi-ccu.c: assert the reset, switch the clock on, release the reset. */
void sunxi_ccu_enable(uintptr_t gate_address, unsigned int gate_bit, uintptr_t reset_address,
	unsigned int reset_bit);

/* pinctrl/pinctrl-sunxi.c: set the pin's function (and pull/drive when they are not 0). */
void sunxi_pinctrl_set(const struct sunxi_pin *pin);
void sunxi_gpio_set_output(const struct sunxi_pin *pin, int value);
int sunxi_gpio_get(const struct sunxi_pin *pin);

/* cst816.c: the capacitive panel on I2C0, polled; one report per call. */
bool sunxi_touch_ready(void);
int sunxi_touch_read(uint16_t *x_pos, uint16_t *y_pos, bool *pressed);

/* tty/serial/uart-dw.c: the 8250-compatible console; TX is polled, RX goes through a queue. */
int sunxi_uart_init(const struct sunxi_uart_config *config);
void sunxi_uart_putc(char character);
int sunxi_uart_getc(int timeout_ms);
int sunxi_uart_start_rx(void);

#endif /* CARDSIGHT_DRIVERS_H */
