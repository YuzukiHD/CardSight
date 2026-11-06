/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The display subsystem: it probes the panel of the board (the ST7789 in st7789.c), publishes
 * the geometry to the rest of the kernel and writes rectangles to it. The LVGL port on top
 * only sees struct sunxi_display_info and sunxi_display_write().
 */

#define LOG_MODULE "display"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm/sunxi_display.h>
#include <hal/kernel.h>
#include <init.h>
#include <log.h>
#include <shell.h>

#include "drivers/display/panel-st7789.h"
#include "drivers/drivers.h"

static bool display_ready;
static struct sunxi_display_info display_info;

static int display_init(void)
{
	int result = st7789_probe(&sunxi_panel0);

	if (result != 0) {
		pr_err("panel probe failed: %d\n", result);
		return result;
	}
	display_info = (struct sunxi_display_info){
		.width = sunxi_panel0.width,
		.height = sunxi_panel0.height,
		.fb_width = sunxi_panel0.width,
		.fb_height = sunxi_panel0.height,
		.stride = sunxi_panel0.width * 3U,
		.bytes_per_pixel = 3U,
		.pixels_swapped = false,
	};
	display_ready = true;
	return 0;
}
INITCALL(2, 30, display_init);

/* ---- the API the UI uses ---- */

bool sunxi_display_ready(void)
{
	return display_ready;
}

int sunxi_display_get_info(struct sunxi_display_info *info)
{
	if (!display_ready) {
		return -ENODEV;
	}
	*info = display_info;
	return 0;
}

int sunxi_display_write(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height, uint16_t pitch,
	const void *buffer)
{
	const uint8_t *rows = buffer;
	int result;

	if (!display_ready || width == 0U || height == 0U || x_pos + width > display_info.width ||
	    y_pos + height > display_info.height) {
		return -EINVAL;
	}
	result = st7789_set_window(x_pos, y_pos, width, height);
	if (result != 0) {
		return result;
	}
	for (uint16_t row = 0U; row < height; row++) {
		result = st7789_write_pixels(rows + (size_t)row * pitch * display_info.bytes_per_pixel,
			(size_t)width * display_info.bytes_per_pixel);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

/* Fill the whole panel with one RGB565 colour, written in the same B, G, R order as LVGL. */
static int display_fill(uint16_t color)
{
	size_t bytes = (size_t)display_info.width * display_info.bytes_per_pixel;
	uint8_t *line = malloc(bytes);
	int result;

	if (line == NULL) {
		return -ENOMEM;
	}
	for (uint32_t index = 0U; index < display_info.width; index++) {
		uint32_t red = ((color >> 11) & 0x1fU) * 255U / 31U;
		uint32_t green = ((color >> 5) & 0x3fU) * 255U / 63U;
		uint32_t blue = (color & 0x1fU) * 255U / 31U;

		line[index * 3U] = (uint8_t)(blue & 0xfcu);
		line[index * 3U + 1U] = (uint8_t)(green & 0xfcu);
		line[index * 3U + 2U] = (uint8_t)(red & 0xfcu);
	}
	result = st7789_set_window(0U, 0U, display_info.width, display_info.height);
	for (uint32_t row = 0U; result == 0 && row < display_info.height; row++) {
		result = st7789_write_pixels(line, bytes);
	}
	free(line);
	return result;
}

#ifdef CONFIG_SHELL
static int command_display(int argc, char **argv)
{
	uint16_t color;
	int result;

	if (!display_ready) {
		printf("no display\n");
		return -ENODEV;
	}
	if (argc == 2 && strcmp(argv[1], "test") == 0) {
		static const uint16_t colors[] = { 0xf800U, 0x07e0U, 0x001fU, 0xffffU, 0x0000U };

		for (uint32_t index = 0U; index < ARRAY_SIZE(colors); index++) {
			result = display_fill(colors[index]);
			if (result != 0) {
				printf("failed: %d\n", result);
				return result;
			}
			printf("colour %04x\n", (uint32_t)colors[index]);
			msleep(700U);
		}
		printf("ok\n");
		return 0;
	}
	if (argc < 3 || strcmp(argv[1], "color") != 0) {
		printf("display color <rgb565> | test\n");
		return -EINVAL;
	}
	color = (uint16_t)strtoul(argv[2], NULL, 16);
	result = display_fill(color);
	printf(result == 0 ? "ok\n" : "failed: %d\n", result);
	return result;
}
SHELL_CMD(display, "display color <rgb565> | test", command_display);
#endif
