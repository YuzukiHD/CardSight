// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file widgets_button.c
 * @brief Press feedback and the glass button styles.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/* Button that fills a bottom bar, with the corners following the glass. */
/**
 * @brief Animation callback that sets a layer's background opacity.
 *
 * @param[in,out] object Layer being animated.
 * @param[in]     value  New opacity, 0..255.
 */
void press_layer_anim_opa(void *object, int32_t value)
{
	lv_obj_set_style_bg_opa(object, (lv_opa_t)value, 0);
}

/* Fade the white layer in and out instead of switching it, so the press reads as a glow. */
/**
 * @brief Fade the press layer to a new opacity instead of switching it.
 *
 * @details Starts a one-shot LVGL animation from the current opacity to
 *          @p target with an ease-out path, so the press reads as a glow.
 *
 * @param[in,out] layer    Layer to fade.
 * @param[in]     target   Target background opacity, 0..255.
 * @param[in]     duration Animation duration in milliseconds.
 */
void press_layer_fade(lv_obj_t *layer, uint8_t target, uint32_t duration)
{
	lv_anim_t animation;

	lv_anim_init(&animation);
	lv_anim_set_var(&animation, layer);
	lv_anim_set_exec_cb(&animation, press_layer_anim_opa);
	lv_anim_set_values(&animation, lv_obj_get_style_bg_opa(layer, 0), target);
	lv_anim_set_duration(&animation, duration);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

/**
 * @brief Press/release handler that brightens or restores the press layer.
 *
 * @details On LV_EVENT_PRESSED the layer fades to opacity 52; on release or
 *          press-lost it fades back to the base opacity stored in the layer's
 *          user data. When animations are disabled the opacity is set directly
 *          instead.
 *
 * @param[in] event LVGL pressed, released, or press-lost event; the layer is
 *                  the user data.
 */
void press_layer_event(lv_event_t *event)
{
	lv_obj_t *layer = lv_event_get_user_data(event);
	bool pressed = lv_event_get_code(event) == LV_EVENT_PRESSED;
	uint8_t base = (uint8_t)(uintptr_t)lv_obj_get_user_data(layer);

	if (ui.animations) {
		press_layer_fade(layer, pressed ? 52 : base, pressed ? 70 : 150);
	} else {
		lv_obj_set_style_bg_opa(layer, pressed ? 52 : base, 0);
	}
}

/* A rounded white layer that brightens the glass on press instead of a rectangular fill. */
/**
 * @brief Add a rounded white press-glow layer inside a button.
 *
 * @details Creates a non-clickable, one-pixel-inset white layer with the
 *          button's radius minus one. The base opacity is stored in the layer's
 *          user data for press_layer_event(), and the layer is wired to the
 *          button's pressed, released, and press-lost events. The label is
 *          raised above the layer when one is supplied.
 *
 * @param[in,out] button   Button owning the layer.
 * @param[in]     label    Button label to keep on top; may be NULL.
 * @param[in]     radius   Button corner radius in pixels.
 * @param[in]     base_opa Resting opacity of the white layer, 0..255.
 */
void add_press_layer(lv_obj_t *button, lv_obj_t *label, int32_t radius, uint8_t base_opa)
{
	int32_t width = lv_obj_get_width(button);
	int32_t height = lv_obj_get_height(button);
	lv_obj_t *layer = lv_obj_create(button);

	lv_obj_set_size(layer, width - 2, height - 2);
	lv_obj_set_pos(layer, 1, 1);
	lv_obj_set_style_bg_color(layer, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_bg_opa(layer, base_opa, 0);
	lv_obj_set_style_border_width(layer, 0, 0);
	lv_obj_set_style_radius(layer, radius > 2 ? radius - 1 : radius, 0);
	lv_obj_set_style_pad_all(layer, 0, 0);
	lv_obj_set_clickable(layer, false);
	lv_obj_set_scrollable(layer, false);
	lv_obj_set_user_data(layer, (void *)(uintptr_t)base_opa);
	lv_obj_add_event_cb(button, press_layer_event, LV_EVENT_PRESSED, layer);
	lv_obj_add_event_cb(button, press_layer_event, LV_EVENT_RELEASED, layer);
	lv_obj_add_event_cb(button, press_layer_event, LV_EVENT_PRESS_LOST, layer);
	if (label != NULL) {
		lv_obj_move_foreground(label);
	}
}

/**
 * @brief Apply the neutral glass pill look used by the bottom bar buttons.
 *
 * @details Switches the button to the ghost style, forces the text to the
 *          primary text colour on both button and label, attaches a glass
 *          surface at the button's radius with the control tint opacity, and
 *          adds a subtle press glow.
 *
 * @param[in,out] button Button to restyle.
 */
void ui_glass_neutral(lv_obj_t *button)
{
	lv_obj_t *label = lv_obj_get_child(button, 0);
	int32_t radius = lv_obj_get_style_radius(button, 0);

	ui_style_button(button, UI_BUTTON_GHOST);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_TEXT), 0);
	if (label != NULL) {
		lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
	}
	glass_attach_tinted(button, radius, UI_GLASS_TINT_OPA);
	add_press_layer(button, label, radius, 20);
}

/**
 * @brief Turn an existing action button into a glass pill with a coloured rim.
 *
 * @details Switches to the ghost style, attaches the frosted highlight sheet,
 *          and re-enables clicking because the highlight helper makes its object
 *          click-through. A 38-opacity accent wash fills the pill, and the rim,
 *          shadow, and text take the accent colour. A press layer with zero base
 *          opacity supplies the glow.
 *
 * @param[in,out] button Button to restyle.
 * @param[in]     color  Accent colour of the rim, shadow, and wash.
 */
void ui_glass_action(lv_obj_t *button, uint32_t color)
{
	lv_obj_t *label = lv_obj_get_child(button, 0);
	int32_t width = lv_obj_get_width(button);
	int32_t height = lv_obj_get_height(button);
	int32_t radius = lv_obj_get_style_radius(button, 0);
	lv_obj_t *accent;

	ui_style_button(button, UI_BUTTON_GHOST);
	glass_highlight_attach(button, radius);
	/* the highlight helper makes its object click-through for the tab indicator; a button
	 * still has to take its presses */
	lv_obj_set_clickable(button, true);
	/* a soft accent wash turns the neutral glass into a primary action */
	accent = lv_obj_create(button);
	lv_obj_set_size(accent, width - 2, height - 2);
	lv_obj_set_pos(accent, 1, 1);
	lv_obj_set_style_bg_color(accent, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(accent, 38, 0);
	lv_obj_set_style_border_width(accent, 0, 0);
	lv_obj_set_style_radius(accent, radius > 2 ? radius - 1 : radius, 0);
	lv_obj_set_style_pad_all(accent, 0, 0);
	lv_obj_set_clickable(accent, false);
	lv_obj_set_scrollable(accent, false);
	if (label != NULL) {
		lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
		lv_obj_move_foreground(label);
	}
	lv_obj_set_style_text_color(button, lv_color_hex(UI_TEXT), 0);
	lv_obj_set_style_border_width(button, 1, 0);
	lv_obj_set_style_border_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_border_opa(button, 160, 0);
	lv_obj_set_style_shadow_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_shadow_opa(button, 60, 0);
	add_press_layer(button, label, radius, 0);
}

/**
 * @brief Create a glass button sized for the bottom action bar.
 *
 * @details Builds a normal button, resizes it to the bar height, gives it the
 *          bar radius, and applies the neutral glass treatment.
 *
 * @param[in] parent   Parent object (the bottom bar).
 * @param[in] text     Button label.
 * @param[in] callback Handler invoked on LV_EVENT_CLICKED.
 * @param[in] width    Button width in pixels.
 * @return The new button object.
 */
lv_obj_t *make_bar_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, int32_t width)
{
	lv_obj_t *button = make_button(parent, text, callback, NULL);

	lv_obj_set_size(button, width, APP_BAR_HEIGHT);
	lv_obj_set_style_radius(button, ui_bar_radius(), 0);
	ui_glass_neutral(button);
	return button;
}
