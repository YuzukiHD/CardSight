/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel: its size and pixel format, and the rectangle writes the UI flushes through.
 */

#ifndef CARDSIGHT_SUNXI_DISPLAY_H
#define CARDSIGHT_SUNXI_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

struct sunxi_display_info {
	uint16_t width, height; /* panel */
	uint16_t fb_width, fb_height;
	uint32_t stride; /* bytes per frame buffer row */
	uint8_t bytes_per_pixel; /* 2: RGB565 */
	bool pixels_swapped; /* RGB565 pixels are byte swapped, as the panel takes them */
};

bool sunxi_display_ready(void);
int sunxi_display_get_info(struct sunxi_display_info *info);

/**
 * Copy a rectangle of @p pitch pixels wide source rows to the panel. @p buf is RGB565 in the byte
 * order the display engine uses (see pixels_swapped); the write goes out over SPI.
 */
int sunxi_display_write(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height,
	uint16_t pitch, const void *buffer);

#endif /* CARDSIGHT_SUNXI_DISPLAY_H */
