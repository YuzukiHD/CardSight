/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file panel-st7789.h
 * @brief The ST7789-compatible panel of the board.
 *
 * Reset, the vendor initialisation table, a window and the pixel stream. It talks to the SPI
 * controller through <spi.h>; the display subsystem in display.c publishes it to the rest of
 * the kernel.
 */

#ifndef CARDSIGHT_ST7789_H
#define CARDSIGHT_ST7789_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <board/cardsight_board.h>

#include "drivers/spi/spi-sunxi.h"

/**
 * @brief Description of one panel instance on the board.
 */
struct sunxi_panel_config {
	/** @brief Width of the visible area in pixels. */
	uint16_t width;
	/** @brief Height of the visible area in pixels. */
	uint16_t height;
	/** @brief Whether the panel is wired with blue and red exchanged. */
	bool bgr;
	/** @brief Whether the column order is mirrored. */
	bool mirror_x;
	/** @brief The data/command line. */
	struct sunxi_pin command;
	/** @brief The reset line. */
	struct sunxi_pin reset;
	/** @brief SPI controller and timing the panel is attached to. */
	const struct sunxi_spi_config *spi;
};

/** @brief The board's only panel, an ST7789 on SPI1. */
extern const struct sunxi_panel_config sunxi_panel0;

/**
 * @brief Reset, initialise and enable the panel.
 *
 * @details Initialises the SPI controller, pulses the reset line and walks the vendor
 * initialisation table, honouring each step's recovery delay.
 *
 * @param[in] config Board data for the panel.
 * @return 0 on success, -EINVAL for a NULL @p config or a missing SPI controller, or -EIO when
 *         a command could not be sent.
 */
int st7789_probe(const struct sunxi_panel_config *config);

/**
 * @brief Program the address window and prepare the panel for pixel data.
 *
 * @details Sends CASET and RASET with the inclusive end coordinates, then RAMWR.
 *
 * @param[in] x_pos  Left column of the rectangle.
 * @param[in] y_pos  Top row of the rectangle.
 * @param[in] width  Rectangle width in pixels.
 * @param[in] height Rectangle height in pixels.
 * @return 0 on success, or a negative errno from the last command.
 */
int st7789_set_window(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height);

/**
 * @brief Send pixels in the panel's byte order.
 *
 * @details One burst with the data/command line high.
 *
 * @param[in] buffer Pixel bytes.
 * @param[in] length Size of @p buffer in bytes.
 * @return 0 on success, or a negative errno from the SPI controller.
 */
int st7789_write_pixels(const void *buffer, size_t length);

#endif /* CARDSIGHT_ST7789_H */
