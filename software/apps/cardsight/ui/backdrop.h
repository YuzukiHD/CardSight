// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file backdrop.h
 * @brief Wallpaper artwork accessors used behind the glass UI.
 *
 * Exposes the raw panel artwork and the derived blurred and darkened variants
 * that the screens, glass surfaces, and text pages are composed from.
 */
#ifndef CARDSIGHT_UI_BACKDROP_H
#define CARDSIGHT_UI_BACKDROP_H

#include <lvgl.h>
#include <stdint.h>

/**
 * @brief Return the panel artwork as an LVGL image descriptor (RGB888).
 *
 * @return Pointer to the static artwork descriptor; never NULL and owned by
 *         the backdrop module.
 */
const lv_image_dsc_t *backdrop_image(void);
/**
 * @brief Return a one-time blurred copy of the artwork, for the frosted glass
 *        interior.
 *
 * @return Pointer to the raw RGB888 pixel buffer of the blurred copy.
 */
const uint8_t *backdrop_blurred(void);
/**
 * @brief Return the same blurred copy as an image, used as the screen backdrop
 *        and by glass highlights.
 *
 * @return Pointer to the static blurred image descriptor; never NULL.
 */
const lv_image_dsc_t *backdrop_blurred_image(void);
/**
 * @brief Return the darkened artwork with the blurred copy already mixed in at
 *        160/255, for text pages.
 *
 * @return Pointer to the static darkened image descriptor; never NULL.
 */
const lv_image_dsc_t *backdrop_text_image(void);

#endif
