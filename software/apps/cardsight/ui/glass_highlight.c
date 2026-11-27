// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file glass_highlight.c
 * @brief Frosted highlight sheets layered over widgets.
 *
 * @details A highlight is a blurred-backdrop image plus a translucent white
 * wash inside a widget, kept aligned with the widget's absolute position.
 * The surface renderer itself lives in @c glass.c.
 */
#include "ui/glass.h"

#include "ui/backdrop.h"

/**
 * @brief Create a non-interactive coloured overlay child.
 *
 * @details Sets a fixed size and position, a flat rounded background, no border
 *          or padding, and disables clicks and scrolling so the overlay never
 *          steals input from its parent.
 *
 * @param[in] parent  Parent object.
 * @param[in] x       Left edge in the parent's coordinate space.
 * @param[in] y       Top edge in the parent's coordinate space.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] radius  Corner radius in pixels.
 * @param[in] color   Background colour, 0xRRGGBB.
 * @param[in] opacity Background opacity, 0..255.
 * @return The new overlay object.
 */
static lv_obj_t *make_overlay(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
	int32_t radius, uint32_t color, uint8_t opacity)
{
	lv_obj_t *overlay = lv_obj_create(parent);

	lv_obj_set_size(overlay, width, height);
	lv_obj_set_pos(overlay, x, y);
	lv_obj_set_style_bg_color(overlay, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(overlay, opacity, 0);
	lv_obj_set_style_border_width(overlay, 0, 0);
	lv_obj_set_style_radius(overlay, radius, 0);
	lv_obj_set_style_pad_all(overlay, 0, 0);
	lv_obj_set_clickable(overlay, false);
	lv_obj_set_scrollable(overlay, false);
	return overlay;
}

/**
 * @brief Attach a frosted highlight sheet to a widget.
 *
 * @details Makes the widget transparent with a one-pixel white border at 110
 *          opacity, clips to the corner radius, and adds a soft white shadow.
 *          A blurred-backdrop image child is positioned so the wallpaper lines
 *          up with the widget's screen position, and a 30-opacity white wash is
 *          laid over it to turn the blur into a glass highlight. The rim itself
 *          is the object border.
 *
 * @param[in] object Widget to highlight; its background is replaced.
 * @param[in] radius Corner radius in pixels.
 * @return The backdrop image child, to be passed to glass_highlight_move() when
 *         the widget moves.
 */
lv_obj_t *glass_highlight_attach(lv_obj_t *object, int32_t radius)
{
	lv_area_t coords;
	lv_obj_t *image;
	int32_t width;
	int32_t height;

	lv_obj_update_layout(object);
	width = lv_obj_get_width(object);
	height = lv_obj_get_height(object);
	/* transparent fill, a fine white rim, and a soft drop shadow */
	lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(object, 1, 0);
	lv_obj_set_style_border_color(object, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_border_opa(object, 110, 0);
	lv_obj_set_style_radius(object, radius, 0);
	lv_obj_set_style_clip_corner(object, true, 0);
	lv_obj_set_style_pad_all(object, 0, 0);
	lv_obj_set_style_shadow_width(object, 14, 0);
	lv_obj_set_style_shadow_color(object, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_shadow_opa(object, 40, 0);
	lv_obj_set_style_shadow_ofs_y(object, 2, 0);
	lv_obj_set_clickable(object, false);
	lv_obj_set_scrollable(object, false);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
	lv_obj_update_layout(object);
	lv_obj_get_coords(object, &coords);

	/* align the blurred wallpaper with the widget's absolute position and send it behind */
	image = lv_image_create(object);
	lv_image_set_src(image, backdrop_blurred_image());
	lv_obj_set_pos(image, -coords.x1 - 1, -coords.y1 - 1);
	lv_obj_set_scrollable(image, false);
	lv_obj_move_background(image);

	/* a light wash turns the blur into a glass highlight; the rim itself is the object border */
	make_overlay(object, 1, 1, width - 2, height - 2, radius > 2 ? radius - 1 : radius, 0xffffff, 30);
	return image;
}

/**
 * @brief Realign the highlight backdrop after its owner moved.
 *
 * @details Repositions the blurred image so its top-left stays one pixel
 *          outside the widget's current absolute coordinates. Null arguments
 *          are ignored so callers do not need to guard teardown.
 *
 * @param[in] object   Widget that owns the highlight.
 * @param[in] backdrop Backdrop child returned by glass_highlight_attach().
 */
void glass_highlight_move(lv_obj_t *object, lv_obj_t *backdrop)
{
	lv_area_t coords;

	if (object == NULL || backdrop == NULL) {
		return;
	}
	lv_obj_get_coords(object, &coords);
	lv_obj_set_pos(backdrop, -coords.x1 - 1, -coords.y1 - 1);
}
