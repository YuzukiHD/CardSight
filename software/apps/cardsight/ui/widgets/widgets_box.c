// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file widgets_box.c
 * @brief Basic containers, cards, chips, buttons and layout metrics.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/**
 * @brief Disable both scroll directions on an object.
 *
 * @param[in,out] object Object to make static.
 */
void disable_scroll(lv_obj_t *object)
{
	lv_obj_set_scrollable(object, false);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

/**
 * @brief Enable vertical scrolling with the themed elastic behaviour.
 *
 * @details Restricts scrolling to the vertical direction and hides the
 *          scrollbar: the reference UI has no visible scrollbars, though the
 *          list still drags and scrolls.
 *
 * @param[in,out] object Object that should scroll vertically.
 */
void enable_vertical_scroll(lv_obj_t *object)
{
	lv_obj_set_scrollable(object, true);
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	/* the reference UI has no visible scrollbars; the list still drags and scrolls */
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

/* Transparent, inert layout container. */
/**
 * @brief Create a transparent, inert layout container.
 *
 * @details Transparent background, no border or padding, not clickable, and
 *          no scrolling; it exists only to group and position children.
 *
 * @param[in] parent Parent object.
 * @param[in] width  Requested width in pixels.
 * @param[in] height Requested height in pixels.
 * @return The new box object.
 */
lv_obj_t *make_box(lv_obj_t *parent, int32_t width, int32_t height)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_set_size(box, width, height);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_clickable(box, false);
	disable_scroll(box);
	return box;
}

/**
 * @brief Create a styled card as a child of @p parent.
 *
 * @details Builds a box and applies the surface colour and the standard card
 *          radius.
 *
 * @param[in] parent Parent object.
 * @param[in] width  Requested width in pixels.
 * @param[in] height Requested height in pixels.
 * @param[in] color  Background colour, 0xRRGGBB.
 * @return The new card object.
 */
lv_obj_t *make_card(lv_obj_t *parent, int32_t width, int32_t height, uint32_t color)
{
	lv_obj_t *card = make_box(parent, width, height);

	ui_style_card(card, color, APP_CARD_RADIUS);
	return card;
}

/*
 * A liquid-glass surface: its own refraction canvas (see glass.c), a one-pixel light edge and a
 * soft shadow. The canvas is the object's background image, so it moves and clips with it.
 */
/**
 * @brief Create a lifted glass sheet with a light edge, specular rim and soft shadow.
 *
 * @details Builds a transparent rounded object with corner clipping, a
 *          one-pixel light border, and a soft black shadow below. The
 *          refraction canvas from glass.c is attached as the object's
 *          background image, so it moves and clips with the sheet.
 *
 * @param[in] parent Parent object.
 * @param[in] x      Left edge in the parent's coordinate space.
 * @param[in] y      Top edge in the parent's coordinate space.
 * @param[in] width  Width in pixels.
 * @param[in] height Height in pixels.
 * @param[in] radius Corner radius in pixels.
 * @return The new glass sheet object.
 */
lv_obj_t *make_glass_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
	int32_t radius)
{
	lv_obj_t *card = lv_obj_create(parent);

	lv_obj_set_size(card, width, height);
	lv_obj_set_pos(card, x, y);
	lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_color(card, lv_color_hex(UI_GLASS_EDGE), 0);
	lv_obj_set_style_border_opa(card, UI_GLASS_EDGE_OPA, 0);
	lv_obj_set_style_radius(card, radius, 0);
	lv_obj_set_style_clip_corner(card, true, 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 10, 0);
	lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
	lv_obj_set_style_shadow_opa(card, 70, 0);
	lv_obj_set_style_shadow_ofs_y(card, 3, 0);
	lv_obj_set_clickable(card, false);
	disable_scroll(card);
	glass_attach(card, radius);
	return card;
}

/* Small rounded tag whose background is the text colour tinted into the surface. */
/**
 * @brief Create a small rounded status tag.
 *
 * @details The tag is a content-sized, 24-pixel-high box holding a coloured
 *          label, with a row flex layout that centres the text. Its background
 *          is the text colour tinted into the surface, with the standard corner
 *          radius of 12 pixels.
 *
 * @param[in] parent Parent object.
 * @param[in] text   Chip text.
 * @param[in] color  Chip accent colour, also used for the label.
 * @return The new chip object.
 */
lv_obj_t *make_chip(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *chip = make_box(parent, LV_SIZE_CONTENT, 24);
	lv_obj_t *label = make_label(chip, text, color);

	ui_style_card(chip, UI_SURFACE, 12);
	lv_obj_set_style_bg_color(chip, ui_tint(color, 40), 0);
	lv_obj_set_style_pad_hor(chip, 7, 0);
	lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	(void)label;
	return chip;
}

/**
 * @brief Create a neutral action button with a centred clipped label.
 *
 * @details The button gets the standard height, the normal button style, and
 *          the card radius. Its label is 94 % of the button width, clipped
 *          rather than wrapped, and centred. When @p callback is non-NULL it is
 *          registered for LV_EVENT_CLICKED with @p data as user data.
 *
 * @param[in] parent   Parent object.
 * @param[in] text     Button label.
 * @param[in] callback Handler invoked on LV_EVENT_CLICKED; may be NULL.
 * @param[in] data     User data passed to @p callback.
 * @return The new button object.
 */
lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_t *label = lv_label_create(button);

	lv_obj_set_height(button, APP_BUTTON_HEIGHT);
	ui_style_button(button, UI_BUTTON_NORMAL);
	lv_obj_set_style_radius(button, APP_CARD_RADIUS, 0);
	disable_scroll(button);
	lv_label_set_text(label, text);
	lv_obj_set_width(label, lv_pct(94));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(label);
	if (callback != NULL) {
		lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
	}
	return button;
}

/* Smallest edge inset that keeps a square content corner inside the rounded glass. */
/**
 * @brief Return the smallest edge inset that keeps content inside the rounded glass.
 *
 * @details Scales the screen radius by roughly 0.3 and adds one pixel.
 *
 * @return Margin in pixels.
 */
int32_t ui_margin(void)
{
	return (ui.screen_radius * 3 + 5) / 10 + 1;
}

/* Corner radius of anything that sits APP_BAR_MARGIN from the edge, concentric with the glass. */
/**
 * @brief Return the corner radius for objects inset by APP_BAR_MARGIN.
 *
 * @details Subtracts the bar margin from the screen radius so the outline stays
 *          concentric with the glass, clamps the result to at least 8 and to
 *          half the bar height.
 *
 * @return Radius in pixels.
 */
int32_t ui_bar_radius(void)
{
	return MIN(MAX(ui.screen_radius - APP_BAR_MARGIN, 8), APP_BAR_HEIGHT / 2);
}

/* Width of controls that sit clear of the corners, such as the docked action button. */
/**
 * @brief Return the width of controls that sit clear of the corners.
 *
 * @return Screen width minus twice the edge padding, in pixels.
 */
int32_t ui_wide_width(void)
{
	return (int32_t)ui.width - 2 * APP_EDGE_PAD;
}

/**
 * @brief Return the usable content width of the current page.
 *
 * @return Screen width minus the margins on both sides, in pixels.
 */
int32_t ui_page_width(void)
{
	return (int32_t)ui.width - 2 * ui_margin();
}

/* Content stops above the bottom bar, which always holds the tabs or the back button. */
/**
 * @brief Return the usable content height of the current page.
 *
 * @details Subtracts the margins, page header, app bar, and the gap that
 *          reserves the bottom bar, which always holds the tabs or the back
 *          button.
 *
 * @return Height in pixels.
 */
int32_t ui_page_height(void)
{
	return (int32_t)ui.height - ui_margin() - APP_HEADER_HEIGHT - APP_BAR_MARGIN - APP_BAR_HEIGHT -
		APP_GAP;
}

/**
 * @brief Enable or disable a widget.
 *
 * @param[in,out] object   Widget to toggle.
 * @param[in]     disabled True to add LV_STATE_DISABLED, false to remove it.
 */
void set_disabled(lv_obj_t *object, bool disabled)
{
	if (disabled) lv_obj_add_state(object, LV_STATE_DISABLED);
	else lv_obj_remove_state(object, LV_STATE_DISABLED);
}
