// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file backdrop.c
 * @brief The artwork behind the UI, generated once at runtime.
 *
 * The source artwork is RGB888 with B, G, R in memory. This file keeps a
 * darkened copy for the screen and a bright blurred copy for the frosted
 * material; both are derived lazily on first use and then cached.
 */

#include "ui/backdrop.h"

#include "assets/background.h"
#include <bits.h>
#include <stdbool.h>

/** @brief Width of the backdrop artwork in pixels. */
#define BACKDROP_WIDTH	240
/** @brief Height of the backdrop artwork in pixels. */
#define BACKDROP_HEIGHT 284
/** @brief Bytes per pixel of the RGB888 artwork. */
#define BACKDROP_BPP	3U
/** @brief Box-blur radius in pixels, sampled on each side. */
#define BLUR_RADIUS	4

/** @brief Darkened copy of the artwork used as the screen background. */
static uint8_t background[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
/** @brief Twice-blurred copy of the artwork used by the glass material. */
static uint8_t blurred[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
/** @brief Blurred copy pre-mixed over the darkened artwork for text pages. */
static uint8_t frosted[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
/** @brief Intermediate buffer reused by the separable blur passes. */
static uint8_t scratch[BACKDROP_WIDTH * BACKDROP_HEIGHT * BACKDROP_BPP];
/** @brief True once the darkened copy has been generated. */
static bool dark_ready;
/** @brief True once the blurred copy has been generated. */
static bool ready;
/** @brief True once the frosted composite has been generated. */
static bool frosted_ready;

/** @brief RGB888 image descriptor wrapping @c background. */
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

/** @brief RGB888 image descriptor wrapping @c blurred. */
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

/** @brief RGB888 image descriptor wrapping @c frosted. */
static lv_image_dsc_t frosted_image = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_RGB888,
		.w = BACKDROP_WIDTH,
		.h = BACKDROP_HEIGHT,
		.stride = BACKDROP_WIDTH * BACKDROP_BPP,
	},
	.data_size = sizeof(frosted),
	.data = frosted,
};

/* A calmer copy of the artwork keeps white text readable over it. */
/**
 * @brief Darken every channel of the artwork to three quarters.
 *
 * @details Runs once. The generated copy is cached in @c background and the
 *          @c dark_ready flag guards later calls.
 */
static void make_dark(void)
{
	const uint8_t *source = cardsight_background.data;

	if (dark_ready) {
		return;
	}
	/* scale each channel by 3/4 so white labels stay legible */
	for (uint32_t index = 0U; index < sizeof(background); index++) {
		background[index] = (uint8_t)(source[index] * 3U / 4U);
	}
	dark_ready = true;
}

/**
 * @brief Run one separable box-blur pass over the artwork.
 *
 * @details For every pixel and channel this averages 2 * BLUR_RADIUS + 1
 *          samples along one axis. Samples near the border are clamped to the
 *          edge pixel, so the result does not darken at the frame. The pass
 *          writes into a target buffer with the transposed line/step indices so
 *          a horizontal pass followed by a vertical pass produces a 2-D blur.
 *
 * @param[in]  source     RGB888 source buffer, BACKDROP_WIDTH * BACKDROP_HEIGHT pixels.
 * @param[out] target     RGB888 destination buffer of the same size.
 * @param[in]  horizontal True for a horizontal pass, false for a vertical one.
 */
static void blur_pass(const uint8_t *source, uint8_t *target, bool horizontal)
{
	int outer = horizontal ? BACKDROP_HEIGHT : BACKDROP_WIDTH;
	int inner = horizontal ? BACKDROP_WIDTH : BACKDROP_HEIGHT;
	int span = 2 * BLUR_RADIUS + 1;

	for (int line = 0; line < outer; line++) {
		for (int step = 0; step < inner; step++) {
			for (uint32_t channel = 0U; channel < BACKDROP_BPP; channel++) {
				uint32_t sum = 0U;

				/* average the clamped window of 2 * BLUR_RADIUS + 1 samples */
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

/**
 * @brief Generate the twice-blurred copy of the artwork.
 *
 * @details Runs once and is guarded by @c ready. The source is blurred
 *          horizontally and vertically twice, ping-ponging through @c scratch,
 *          which gives a softer material than a single 2-D pass would. The
 *          frosted material keeps the original colour; only the screen
 *          background is darkened.
 */
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

/**
 * @brief Return the darkened artwork image, generating it on first use.
 *
 * @return Descriptor of the darkened RGB888 artwork.
 */
const lv_image_dsc_t *backdrop_image(void)
{
	make_dark();
	return &dark_image;
}

/**
 * @brief Return the blurred artwork pixels, generating them on first use.
 *
 * @return Raw RGB888 pixel buffer of the blurred artwork.
 */
const uint8_t *backdrop_blurred(void)
{
	make_blurred();
	return blurred;
}

/**
 * @brief Return the blurred artwork as an image descriptor.
 *
 * @return Descriptor of the blurred RGB888 artwork.
 */
const lv_image_dsc_t *backdrop_blurred_image(void)
{
	make_blurred();
	return &blurred_image;
}

/* The blurred copy laid over the darkened artwork at the old layer opacity. Drawing it as one
 * image instead of blending two full-screen layers every frame leaves the pixels unchanged and
 * takes the 160/255 blend off the render path. */
/**
 * @brief Return the blurred copy pre-mixed over the darkened artwork at 160/255.
 *
 * @details The composite is built once into @c frosted and cached. Drawing it
 *          as a single image instead of blending two full-screen layers every
 *          frame leaves the pixels unchanged and takes the 160/255 blend off
 *          the render path.
 *
 * @return Descriptor of the frosted composite image.
 */
const lv_image_dsc_t *backdrop_text_image(void)
{
	make_dark();
	make_blurred();
	if (!frosted_ready) {
		/* 160/255 of the blur over the remaining 95/255 of the darkened artwork */
		for (size_t index = 0U; index < sizeof(frosted); index++) {
			frosted[index] = (uint8_t)(((uint32_t)blurred[index] * 160U +
						   (uint32_t)background[index] * 95U) >> 8);
		}
		frosted_ready = true;
	}
	return &frosted_image;
}
