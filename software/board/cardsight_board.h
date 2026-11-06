/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The CardSight board in C: base addresses, the console, and later the card slot, panel, touch
 * controller and USB port. There is no device tree anywhere in this tree.
 */

#ifndef CARDSIGHT_BOARD_H
#define CARDSIGHT_BOARD_H

#include <stdint.h>

#define SUNXI_CCU_BASE	  0x02001000UL
#define SUNXI_GPIO_BASE	  0x02000000UL
#define SUNXI_PLIC_BASE	  0x30000000UL
#define SUNXI_PLIC_NDEV	  150U
#define SUNXI_CLINT_BASE	  0x34000000UL
#define SUNXI_TIMEBASE_HZ	  24000000UL
#define SUNXI_PSRAM_BASE	  0x12000000UL
#define SUNXI_PSRAM_SIZE	  0x00800000UL

/* The USB transfer buffer the application owns; the E907 must be held in reset to use it. */
#define SUNXI_MSC_BUFFER_ADDRESS 0x00100000UL
#define SUNXI_MSC_BUFFER_SIZE    0x00010000UL

/* The other half of the 128 KiB boot SRAM: buffers written every frame live here, because
 * stores to the PSRAM run at about 11 MB/s while the SRAM takes them at over 400 MB/s. The
 * second hart pushes the pixels out of these buffers while the first renders the next one. */
#define SUNXI_SRAM_BUFFER_ADDRESS   0x00110000UL
#define SUNXI_SRAM_BUFFER_SIZE	    0x00010000UL
#define SUNXI_PANEL_MAILBOX_ADDRESS 0x0011F100UL

/* The panel and its touch digitizer cover the same glass. */
#define SUNXI_PANEL_WIDTH	240U
#define SUNXI_PANEL_HEIGHT 284U
#define SUNXI_TOUCH_WIDTH	SUNXI_PANEL_WIDTH
#define SUNXI_TOUCH_HEIGHT SUNXI_PANEL_HEIGHT

/* Pins: port index (PA = 0), pin number, function, pull, drive level. */
struct sunxi_pin {
	uint8_t port;
	uint8_t pin;
	uint8_t function;
	uint8_t pull;
	uint8_t drive;
};

struct sunxi_uart_config {
	uintptr_t base;
	uint32_t baud;
	uint32_t clock_hz;
	uint32_t irq; /* PLIC source */
	struct sunxi_pin tx;
	struct sunxi_pin rx;
	uintptr_t gate_address;
	uintptr_t reset_address;
	uint8_t gate_bit;
	uint8_t reset_bit;
};

struct sunxi_mmc_config;

extern const struct sunxi_uart_config sunxi_uart0;

#endif /* CARDSIGHT_BOARD_H */
