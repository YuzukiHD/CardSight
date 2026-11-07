// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file glass.h
 * @brief Liquid-glass surfaces built from canvas refraction of the backdrop.
 *
 * Each helper decorates a widget that has already been positioned and made
 * transparent: it inserts a child canvas that samples the blurred wallpaper,
 * clips it to the widget's rounded shape, and optionally adds rim highlights.
 */
#ifndef CARDSIGHT_UI_GLASS_H
#define CARDSIGHT_UI_GLASS_H

#include <lvgl.h>

/**
 * @brief Attach a refraction canvas to an already positioned, transparent
 *        widget.
 *
 * @param[in] object Target widget; it receives the canvas as first child.
 * @param[in] radius Corner radius of the refraction shape in pixels.
 */
void glass_attach(lv_obj_t *object, int32_t radius);
/**
 * @brief Same as glass_attach(), with an explicit tint strength (0..255) for
 *        content sheets.
 *
 * @param[in] object  Target widget; it receives the canvas as first child.
 * @param[in] radius  Corner radius of the refraction shape in pixels.
 * @param[in] opacity Tint strength of the glass layer, 0 (clear) to 255.
 */
void glass_attach_tinted(lv_obj_t *object, int32_t radius, uint8_t opacity);
/**
 * @brief Sweep a bright glint along the top edge of a glass surface.
 *
 * @param[in] object Glass widget whose top edge is highlighted.
 */
void glass_glint(lv_obj_t *object);
/**
 * @brief Attach a frosted-glass highlight sheet.
 *
 * @param[in] object Glass widget that owns the sheet.
 * @param[in] radius Corner radius of the sheet in pixels.
 * @return The backdrop child object that must be kept aligned when @p object
 *         moves; pass it to glass_highlight_move().
 */
lv_obj_t *glass_highlight_attach(lv_obj_t *object, int32_t radius);
/**
 * @brief Realign a highlight backdrop after its owner moved or resized.
 *
 * @param[in] object   Glass widget that owns the highlight.
 * @param[in] backdrop Backdrop child previously returned by
 *                     glass_highlight_attach() for @p object.
 */
void glass_highlight_move(lv_obj_t *object, lv_obj_t *backdrop);

#endif
