// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The artwork behind the UI, RGB888 (B, G, R in memory): a darkened copy for the screen and a
 * bright blurred copy for the frosted material. Both are made once.
 */

#include "ui/backdrop.h"

#include "assets/background.h"
#include <bits.h>
#include <stdbool.h>

#define BACKDROP_WIDTH	240
#define BACKDROP_HEIGHT 284
#define BACKDROP_BPP	3U
#define BLUR_RADIUS	4

static uint8_t background[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
static uint8_t blurred[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
static uint8_t scratch[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
static bool dark_ready;
static bool ready;

static lv_image_dsc_t dark_image = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_RGB888,
		.w = BACKDROP_WIDTH,
		.h = BACKDROP_HEIGHT,
		.stride = BACKDROP_WIDTH * BACKDROP_BPP,
	},
	.data_size = sizeof(background),
	.data = background,
};

static lv_image_dsc_t blurred_image = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_RGB888,
		.w = BACKDROP_WIDTH,
		.h = BACKDROP_HEIGHT,
		.stride = BACKDROP_WIDTH * BACKDROP_BPP,
	},
	.data_size = sizeof(blurred),
	.data = blurred,
};

/* A calmer copy of the artwork keeps white text readable over it. */
static void make_dark(void)
{
	const uint8_t *source = cardsight_background.data;

	if (dark_ready) {
		return;
	}
	for (uint32_t index = 0U; index < sizeof(background); index++) {
		background[index] = (uint8_t)(source[index] * 3U / 4U);
	}
	dark_ready = true;
}

static void blur_pass(const uint8_t *source, uint8_t *target, bool horizontal)
{
	int outer = horizontal ? BACKDROP_HEIGHT : BACKDROP_WIDTH;
	int inner = horizontal ? BACKDROP_WIDTH : BACKDROP_HEIGHT;
	int span = 2 * BLUR_RADIUS + 1;

	for (int line = 0; line < outer; line++) {
		for (int step = 0; step < inner; step++) {
			for (uint32_t channel = 0U; channel < BACKDROP_BPP; channel++) {
				uint32_t sum = 0U;

				for (int offset = -BLUR_RADIUS; offset <= BLUR_RADIUS; offset++) {
					int sample = step + offset;
					int x;
					int y;

					if (sample < 0) {
						sample = 0;
					} else if (sample >= inner) {
						sample = inner - 1;
					}
					x = horizontal ? sample : line;
					y = horizontal ? line : sample;
					sum += source[((size_t)y * BACKDROP_WIDTH + x) * BACKDROP_BPP + channel];
				}
				target[((size_t)(horizontal ? line : step) * BACKDROP_WIDTH +
					(horizontal ? step : line)) * BACKDROP_BPP + channel] =
					(uint8_t)(sum / span);
			}
		}
	}
}

static void make_blurred(void)
{
	if (ready) {
		return;
	}
	/* the frosted material keeps the original colour; only the screen is darkened */
	blur_pass(cardsight_background.data, scratch, true);
	blur_pass(scratch, blurred, false);
	blur_pass(blurred, scratch, true);
	blur_pass(scratch, blurred, false);
	ready = true;
}

const lv_image_dsc_t *backdrop_image(void)
{
	make_dark();
	return &dark_image;
}

const uint8_t *backdrop_blurred(void)
{
	make_blurred();
	return blurred;
}

const lv_image_dsc_t *backdrop_blurred_image(void)
{
	make_blurred();
	return &blurred_image;
}
