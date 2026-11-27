// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file screenshot.c
 * @brief Serial-console screen dump for inspecting the UI without photographing the screen.
 *
 * An RGB565 snapshot of the active LVGL screen is run-length encoded and
 * printed as base64 between "SHOT" and "END" markers. The capture itself runs
 * in the LVGL task; the shell command only waits, encodes, and prints the
 * result.
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

/** @brief Largest screen width the static capture buffers can hold. */
#define SHOT_MAX_WIDTH	240U
/** @brief Largest screen height the static capture buffers can hold. */
#define SHOT_MAX_HEIGHT 284U

/* Static buffers: the capture runs after the UI has churned the C library heap for a while, and
 * a 136 KiB contiguous allocation is not guaranteed to be available any more. */
/** @brief RGB565 pixel buffer written by the snapshot; aligned for the draw code. */
static uint8_t shot_pixels[SHOT_MAX_WIDTH * SHOT_MAX_HEIGHT * 2U] __attribute__((aligned(64)));
/** @brief Run-length encoded bytes of the last capture, ready for base64 printing. */
static uint8_t shot_encoded[SHOT_MAX_WIDTH * SHOT_MAX_HEIGHT * 3U];
/**
 * @brief Shared state between the shell command and the LVGL-task capture.
 */
static struct {
	/** @brief Captured screen width in pixels. */
	uint32_t width;
	/** @brief Captured screen height in pixels. */
	uint32_t height;
	/** @brief Outcome: 0 ok, 1 screen too large, 2 render failed. */
	volatile int stage; /* 0: ok, 1: screen too large, 2: render failed */
	/** @brief Set when the capture has finished, successfully or not. */
	volatile bool done;
} capture;

/**
 * @brief Capture callback that runs inside the LVGL task.
 *
 * @details Waits for the port to go idle, rejects screens larger than the
 *          static buffers, then initialises a draw buffer over @c shot_pixels
 *          and snapshots the active screen into it as RGB565. The outcome is
 *          published through @c capture.stage and @c capture.done, which the
 *          shell command polls.
 *
 * @param[in] argument Unused; posted via lvgl_port_post().
 */
static void capture_screen(void *argument)
{
	lv_obj_t *screen = lv_screen_active();
	lv_draw_buf_t draw_buf;
	uint32_t width;
	uint32_t height;

	(void)argument;
	lvgl_port_wait_idle();
	width = (uint32_t)lv_obj_get_width(screen);
	height = (uint32_t)lv_obj_get_height(screen);
	/* refuse screens the static buffers cannot hold */
	if (width == 0U || height == 0U || width > SHOT_MAX_WIDTH || height > SHOT_MAX_HEIGHT) {
		capture.stage = 1;
		capture.done = true;
		return;
	}
	capture.width = width;
	capture.height = height;
	if (lv_draw_buf_init(&draw_buf, width, height, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO,
		    shot_pixels, sizeof(shot_pixels)) != LV_RESULT_OK ||
		lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, &draw_buf) != LV_RESULT_OK) {
		capture.stage = 2;
	}
	capture.done = true;
}

/**
 * @brief Run-length encode an RGB565 image into count/colour triples.
 *
 * @details Scans rows in order and emits (count, low byte, high byte) triples.
 *          A run ends at 255 pixels, on a colour change, or at the end of the
 *          image. The encoder returns 0 rather than truncating when the output
 *          cannot hold the next triple.
 *
 * @param[in]  pixels   RGB565 source rows.
 * @param[in]  width    Image width in pixels.
 * @param[in]  height   Image height in pixels.
 * @param[in]  stride   Bytes between successive rows.
 * @param[out] output   Buffer receiving the encoded triples.
 * @param[in]  capacity Size of @p output in bytes.
 * @return Number of bytes written, or 0 when the output buffer is too small.
 */
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
			/* RGB565 is little-endian in the draw buffer */
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
	/* flush the final run */
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

/**
 * @brief Print a byte buffer as base64 with 96-column line wrapping.
 *
 * @details Emits four characters per three input bytes, padding the final
 *          group with '=' after a newline every 96 columns and a final newline
 *          when the last line is partial.
 *
 * @param[in] data   Bytes to encode.
 * @param[in] length Number of bytes in @p data.
 */
static void print_base64(const uint8_t *data, size_t length)
{
	/** @brief Base64 alphabet. */
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

/**
 * @brief Shell command that captures, encodes, and prints the screen.
 *
 * @details Posts capture_screen() to the LVGL task and polls @c capture.done
 *          for up to three seconds. On success it encodes the pixels, prints
 *          "SHOT <width> <height> <bytes>", emits the base64 payload, and ends
 *          with "END".
 *
 * @param[in] argc Unused argument count.
 * @param[in] argv Unused argument vector.
 * @retval 0        The image was printed.
 * @retval -EAGAIN  The LVGL port was not ready.
 * @retval -ETIMEDOUT The capture did not finish in time.
 * @retval -ENOMEM  The screen was too large or rendering failed.
 * @retval -EIO     The encoded image overflowed the output buffer.
 */
static int command_screenshot(int argc, char **argv)
{
	uint32_t width;
	uint32_t height;
	size_t size;

	(void)argc;
	(void)argv;
	capture.stage = 0;
	capture.done = false;
	if (lvgl_port_post(capture_screen, NULL) != 0) {
		printf("lvgl not ready\n");
		return -EAGAIN;
	}
	/* poll until the LVGL task finishes or the three-second budget expires */
	for (uint32_t waited = 0U; waited < 3000U && !capture.done; waited += 10U) {
		msleep(10U);
	}
	if (!capture.done) {
		printf("snapshot timeout\n");
		return -ETIMEDOUT;
	}
	if (capture.stage != 0) {
		printf("snapshot failed (stage %d)\n", capture.stage);
		return -ENOMEM;
	}
	width = capture.width;
	height = capture.height;
	size = rle_encode(shot_pixels, width, height, width * 2U, shot_encoded, sizeof(shot_encoded));
	if (size == 0U) {
		printf("encode failed\n");
		return -EIO;
	}
	printf("SHOT %u %u %u\n", width, height, (uint32_t)size);
	print_base64(shot_encoded, size);
	printf("END\n");
	return 0;
}
/** @brief Registers the screenshot command with the shell. */
SHELL_CMD(screenshot, "dump the screen as RLE/base64", command_screenshot);
