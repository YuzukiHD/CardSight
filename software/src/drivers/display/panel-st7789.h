/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The ST7789-compatible panel of the board: reset, the vendor initialisation table, a window
 * and the pixel stream. It talks to the SPI controller through <spi.h>; the display subsystem
 * in display.c publishes it to the rest of the kernel.
 */

#ifndef CARDSIGHT_ST7789_H
#define CARDSIGHT_ST7789_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <board/cardsight_board.h>

#include "drivers/spi/spi-sunxi.h"

struct sunxi_panel_config {
	uint16_t width;
	uint16_t height;
	bool bgr;
	bool mirror_x;
	struct sunxi_pin command; /* the data/command line */
	struct sunxi_pin reset;
	const struct sunxi_spi_config *spi;
};

extern const struct sunxi_panel_config sunxi_panel0;

int st7789_probe(const struct sunxi_panel_config *config);
int st7789_set_window(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height);
/** Pixels in the panel's byte order, one burst with the data/command line high. */
int st7789_write_pixels(const void *buffer, size_t length);

#endif /* CARDSIGHT_ST7789_H */
