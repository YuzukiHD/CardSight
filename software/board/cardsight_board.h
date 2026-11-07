/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file cardsight_board.h
 * @brief The CardSight board in C: base addresses, the console, and later the card slot, panel,
 * touch controller and USB port.
 *
 * There is no device tree anywhere in this tree.
 */

#ifndef CARDSIGHT_BOARD_H
#define CARDSIGHT_BOARD_H

#include <stdint.h>

/** @brief Base address of the clock control unit. */
#define SUNXI_CCU_BASE	  0x02001000UL
/** @brief Base address of the pin controller. */
#define SUNXI_GPIO_BASE	  0x02000000UL
/** @brief Base address of the platform-level interrupt controller. */
#define SUNXI_PLIC_BASE	  0x30000000UL
/** @brief Number of interrupt sources the PLIC manages. */
#define SUNXI_PLIC_NDEV	  150U
/** @brief Base address of the core-local interruptor and timer. */
#define SUNXI_CLINT_BASE	  0x34000000UL
/** @brief Frequency of the timer that drives ktime, in Hz. */
#define SUNXI_TIMEBASE_HZ	  24000000UL
/** @brief Base address of the PSRAM window. */
#define SUNXI_PSRAM_BASE	  0x12000000UL
/** @brief Size of the PSRAM window in bytes. */
#define SUNXI_PSRAM_SIZE	  0x00800000UL

/**
 * @brief The USB transfer buffer the application owns; the E907 must be held in reset to use
 * it.
 */
#define SUNXI_MSC_BUFFER_ADDRESS 0x00100000UL
/** @brief Size of the USB transfer buffer in bytes. */
#define SUNXI_MSC_BUFFER_SIZE    0x00010000UL

/**
 * @brief The other half of the 128 KiB boot SRAM, where buffers written every frame live.
 *
 * Stores to the PSRAM run at about 11 MB/s while the SRAM takes them at over 400 MB/s. The
 * second hart pushes the pixels out of these buffers while the first renders the next one.
 */
#define SUNXI_SRAM_BUFFER_ADDRESS   0x00110000UL
/** @brief Size of one SRAM frame buffer in bytes. */
#define SUNXI_SRAM_BUFFER_SIZE	    0x00010000UL
/** @brief Address of the mailbox shared with the second hart. */
#define SUNXI_PANEL_MAILBOX_ADDRESS 0x0011F100UL

/** @brief Panel width in pixels; the touch digitizer covers the same glass. */
#define SUNXI_PANEL_WIDTH	240U
/** @brief Panel height in pixels; the touch digitizer covers the same glass. */
#define SUNXI_PANEL_HEIGHT 284U
/** @brief Touch digitizer width in pixels, the same as the panel's. */
#define SUNXI_TOUCH_WIDTH	SUNXI_PANEL_WIDTH
/** @brief Touch digitizer height in pixels, the same as the panel's. */
#define SUNXI_TOUCH_HEIGHT SUNXI_PANEL_HEIGHT

/**
 * @brief One pin: port index (PA = 0), pin number, function, pull, and drive level.
 */
struct sunxi_pin {
	uint8_t port; /**< Port index; PA is 0. */
	uint8_t pin; /**< Pin number within the port. */
	uint8_t function; /**< Pin function number. */
	uint8_t pull; /**< Pull configuration value. */
	uint8_t drive; /**< Drive strength level. */
};

/**
 * @brief Description of one UART instance.
 */
struct sunxi_uart_config {
	uintptr_t base; /**< Register base address. */
	uint32_t baud; /**< Baud rate. */
	uint32_t clock_hz; /**< Input clock frequency in Hz. */
	uint32_t irq; /**< PLIC source number. */
	struct sunxi_pin tx; /**< Transmit pin. */
	struct sunxi_pin rx; /**< Receive pin. */
	uintptr_t gate_address; /**< Clock gate register. */
	uintptr_t reset_address; /**< Reset register. */
	uint8_t gate_bit; /**< Bit that gates the clock. */
	uint8_t reset_bit; /**< Bit that asserts the reset. */
};

/** @brief Description of one MMC host (defined in the mmc driver header). */
struct sunxi_mmc_config;

/** @brief The console UART: UART0 on PH13 and PH14. */
extern const struct sunxi_uart_config sunxi_uart0;

#endif /* CARDSIGHT_BOARD_H */
