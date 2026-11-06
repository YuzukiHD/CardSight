/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SPI controller of the board, polled through its FIFO. One controller (SPI1) is present;
 * the bus description is a board array and lives with the board.
 */

#ifndef CARDSIGHT_SPI_H
#define CARDSIGHT_SPI_H

#include <stddef.h>
#include <stdint.h>

#include <board/cardsight_board.h>

struct sunxi_spi_config {
	uintptr_t base;
	uint32_t module_clock_register; /* CCU offset: 0x944 */
	uintptr_t gate_address;		/* CCU + 0x96c: bus clock, reset */
	uint8_t gate_bit;
	uint8_t reset_bit;
	uint32_t frequency; /* SCK */
	struct sunxi_pin clock;
	struct sunxi_pin mosi;
	struct sunxi_pin cs;
};

extern const struct sunxi_spi_config sunxi_spi1;

/** Pins, clocks and the controller's mode 0 write path. */
int sunxi_spi_init(const struct sunxi_spi_config *config);

/** Send @p length bytes on MOSI; the chip select is driven by the controller. */
int sunxi_spi_write(const void *buffer, size_t length);

#endif /* CARDSIGHT_SPI_H */
