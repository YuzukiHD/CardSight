/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file display.c
 * @brief The display subsystem: panel probe, geometry and rectangle writes.
 *
 * It probes the panel of the board (the ST7789 in panel-st7789.c), publishes the geometry to
 * the rest of the kernel and writes rectangles to it. The LVGL port on top only sees
 * struct sunxi_display_info and sunxi_display_write().
 */

/** @brief Log module name used by the pr_*() helpers. */
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
#include "drivers/display/panel-worker.h"
#include "drivers/drivers.h"

/** @brief Set once the panel probed; gates every API call. */
static bool display_ready;
/** @brief Geometry published to the rest of the kernel. */
static struct sunxi_display_info display_info;

/**
 * @brief Probe the board's panel and publish its geometry.
 *
 * @details The framebuffer matches the panel exactly: three bytes per pixel, no row padding
 * and no channel swap, because the panel is initialised for RGB666 in the B, G, R order the
 * renderer writes. Any probe failure leaves the subsystem unready.
 *
 * @return 0 on success, or the error from the panel probe.
 */
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

/**
 * @brief Report whether a usable display is present.
 *
 * @return true once the panel probed successfully.
 */
bool sunxi_display_ready(void)
{
	return display_ready;
}

/**
 * @brief Copy the published display geometry.
 *
 * @param[out] info Structure receiving the geometry.
 * @return 0 on success, or -ENODEV when no display is present.
 */
int sunxi_display_get_info(struct sunxi_display_info *info)
{
	if (!display_ready) {
		return -ENODEV;
	}
	*info = display_info;
	return 0;
}

/**
 * @brief Write a rectangle of pixels to the panel synchronously.
 *
 * @details Validates the rectangle against the published geometry and programs the panel
 * window once. A pitch equal to the width means the rows are packed, so the whole rectangle is
 * one transfer; a wider pitch (a slice of a larger framebuffer) skips the padding one row at a
 * time, relying on the panel advancing its own address between rows.
 *
 * @param[in] x_pos  Left column.
 * @param[in] y_pos  Top row.
 * @param[in] width  Rectangle width in pixels.
 * @param[in] height Rectangle height in pixels.
 * @param[in] pitch  Row stride in pixels.
 * @param[in] buffer Pixel bytes in the panel's order.
 * @return 0 on success, -EINVAL for an empty or out-of-range rectangle, or a transfer error.
 */
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
	/* the panel advances the address itself, so a packed rectangle is one transfer */
	if (pitch == width) {
		return st7789_write_pixels(buffer, (size_t)width * height * display_info.bytes_per_pixel);
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

/**
 * @brief Hand a rectangle to the worker on hart 1.
 *
 * @details Refuses an unpacked buffer: the worker sends one contiguous run, so the caller must
 * provide a rectangle with pitch equal to its width. The window is programmed on this hart
 * first, then the buffer is handed over and the call returns immediately.
 *
 * @param[in] x_pos  Left column.
 * @param[in] y_pos  Top row.
 * @param[in] width  Rectangle width in pixels.
 * @param[in] height Rectangle height in pixels.
 * @param[in] pitch  Row stride in pixels; must equal @p width.
 * @param[in] buffer Pixel bytes, valid until the transfer reports done.
 * @return 0 on success, -EINVAL for a bad rectangle, -ENODEV without a worker, or -EBUSY when
 *         the previous transfer is still running.
 */
int sunxi_display_write_async(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height,
	uint16_t pitch, const void *buffer)
{
	int result;

	if (!display_ready || width == 0U || height == 0U || pitch != width ||
	    x_pos + width > display_info.width || y_pos + height > display_info.height) {
		return -EINVAL;
	}
	result = st7789_set_window(x_pos, y_pos, width, height);
	if (result != 0) {
		return result;
	}
	return sunxi_panel_worker_start(buffer,
		(size_t)width * height * display_info.bytes_per_pixel);
}

/**
 * @brief Report whether the last asynchronous write finished.
 *
 * @param[out] result Result of the transfer when it did.
 * @return true once, when a transfer has completed since the last call.
 */
bool sunxi_display_write_done(int *result)
{
	return sunxi_panel_worker_done(result);
}

/**
 * @brief Wait until the worker has finished the handed-over pixels.
 *
 * @details Does nothing when no display is present.
 */
void sunxi_display_wait(void)
{
	if (display_ready) {
		sunxi_panel_worker_wait();
	}
}

/**
 * @brief Fill the whole panel with one RGB565 colour.
 *
 * @details Expands the colour to RGB888, truncating the low two bits of each channel to the
 * panel's 6-bit input, then writes one preformatted line to every row of a full-panel window.
 * Allocating the line keeps the caller's stack out of the picture; the transfer is repeated
 * rather than built once because the panel streams while it receives.
 *
 * @param[in] color Colour in RGB565, as LVGL stores it.
 * @return 0 on success, -ENOMEM without a line buffer, or a transfer error.
 */
static int display_fill(uint16_t color)
{
	size_t bytes = (size_t)display_info.width * display_info.bytes_per_pixel;
	uint8_t *line = malloc(bytes);
	int result;

	if (line == NULL) {
		return -ENOMEM;
	}
	/* one line, in the same B, G, R order as LVGL writes */
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
/**
 * @brief Shell command: report the state of the hart 1 pixel pusher.
 *
 * @param[in] argc Argument count, unused.
 * @param[in] argv Argument vector, unused.
 * @return Always 0.
 */
static int command_panel(int argc, char **argv)
{
	uint32_t alive;
	uint32_t command;
	uint32_t heartbeat;

	(void)argc;
	(void)argv;
	sunxi_panel_worker_status(&alive, &command, &heartbeat);
	printf("panel: worker %s, command %u, heartbeat %u\n", alive != 0U ? "alive" : "off",
		(uint32_t)command, (uint32_t)heartbeat);
	return 0;
}
SHELL_CMD(panel, "panel: the state of the second hart's pixel pusher", command_panel);

/**
 * @brief Shell command: fill the panel with a colour, or cycle a test pattern.
 *
 * @details "display color <rgb565>" fills once; "display test" walks red, green, blue, white
 * and black with a 700 ms pause, which is enough to see that the panel is alive.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or a negative errno when no display is present or a fill failed.
 */
static int command_display(int argc, char **argv)
{
	uint16_t color;
	int result;

	if (!display_ready) {
		printf("no display\n");
		return -ENODEV;
	}
	if (argc == 2 && strcmp(argv[1], "test") == 0) {
		/** @brief Colours of the test pattern: red, green, blue, white, black. */
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
