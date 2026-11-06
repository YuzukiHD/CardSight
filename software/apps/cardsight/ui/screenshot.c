// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dump the LVGL screen over the serial console: an RGB565 snapshot is run-length encoded and
 * printed as base64, so the UI can be inspected without a camera. The capture itself runs in the
 * LVGL task; the shell only encodes and prints the result.
 */

#include <gui/lvgl_port.h>
#include <hal/kernel.h>
#include <lvgl.h>
#include <shell.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
	lv_draw_buf_t *buffer;
	volatile bool done;
} capture;

static void capture_screen(void *argument)
{
	(void)argument;
	capture.buffer = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
	capture.done = true;
}

static size_t rle_encode(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride,
	uint8_t *output, size_t capacity)
{
	size_t used = 0U;
	uint16_t previous = 0U;
	uint32_t count = 0U;
	bool started = false;

	for (uint32_t y = 0U; y < height; y++) {
		const uint8_t *row = pixels + (size_t)y * stride;

		for (uint32_t x = 0U; x < width; x++) {
			uint16_t color = (uint16_t)(row[x * 2U] | (row[x * 2U + 1U] << 8));

			if (!started) {
				previous = color;
				count = 1U;
				started = true;
			} else if (color == previous && count < 255U) {
				count++;
			} else {
				if (used + 3U > capacity) {
					return 0U;
				}
				output[used++] = (uint8_t)count;
				output[used++] = (uint8_t)(previous & 0xffU);
				output[used++] = (uint8_t)(previous >> 8);
				previous = color;
				count = 1U;
			}
		}
	}
	if (started) {
		if (used + 3U > capacity) {
			return 0U;
		}
		output[used++] = (uint8_t)count;
		output[used++] = (uint8_t)(previous & 0xffU);
		output[used++] = (uint8_t)(previous >> 8);
	}
	return used;
}

static void print_base64(const uint8_t *data, size_t length)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint32_t column = 0U;

	for (size_t index = 0U; index < length; index += 3U) {
		size_t chunk = length - index < 3U ? length - index : 3U;
		uint32_t value = (uint32_t)data[index] << 16;

		if (chunk > 1U) {
			value |= (uint32_t)data[index + 1U] << 8;
		}
		if (chunk > 2U) {
			value |= data[index + 2U];
		}
		putchar(table[(value >> 18) & 0x3fU]);
		putchar(table[(value >> 12) & 0x3fU]);
		putchar(chunk > 1U ? table[(value >> 6) & 0x3fU] : '=');
		putchar(chunk > 2U ? table[value & 0x3fU] : '=');
		column += 4U;
		if (column >= 96U) {
			putchar('\n');
			column = 0U;
		}
	}
	if (column != 0U) {
		putchar('\n');
	}
}

static int command_screenshot(int argc, char **argv)
{
	lv_draw_buf_t *buffer;
	uint8_t *encoded;
	uint32_t width;
	uint32_t height;
	size_t capacity;
	size_t size;

	(void)argc;
	(void)argv;
	capture.buffer = NULL;
	capture.done = false;
	if (lvgl_port_post(capture_screen, NULL) != 0) {
		printf("lvgl not ready\n");
		return -EAGAIN;
	}
	for (uint32_t waited = 0U; waited < 3000U && !capture.done; waited += 10U) {
		msleep(10U);
	}
	if (!capture.done) {
		printf("snapshot timeout\n");
		return -ETIMEDOUT;
	}
	buffer = capture.buffer;
	if (buffer == NULL) {
		printf("snapshot failed\n");
		return -ENOMEM;
	}
	width = buffer->header.w;
	height = buffer->header.h;
	capacity = (size_t)width * height * 3U;
	encoded = malloc(capacity);
	if (encoded == NULL) {
		lv_draw_buf_destroy(buffer);
		printf("no memory\n");
		return -ENOMEM;
	}
	size = rle_encode(buffer->data, width, height, buffer->header.stride, encoded, capacity);
	lv_draw_buf_destroy(buffer);
	if (size == 0U) {
		free(encoded);
		printf("encode failed\n");
		return -EIO;
	}
	printf("SHOT %u %u %u\n", (uint32_t)width, (uint32_t)height, (uint32_t)size);
	print_base64(encoded, size);
	printf("END\n");
	free(encoded);
	return 0;
}
SHELL_CMD(screenshot, "dump the screen as RLE/base64", command_screenshot);
