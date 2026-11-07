// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file theme.c
 * @brief Colour mixing and widget styling for the CardSight UI.
 *
 * Button styling writes every visual property for every state the widget can
 * enter, so no default-theme decoration leaks through on press, focus, or
 * disable. Cards and scrollbars get the flat navy/ice treatment.
 */
#include "ui/theme.h"
#include <bits.h>

/* Mix a colour into the surface tone; amount 0 is the surface, 255 is the colour. */
/**
 * @brief Mix a colour into the surface tone.
 *
 * @param[in] color  Colour to mix in, 0xRRGGBB.
 * @param[in] amount Weight of @p color; 0 is the surface, 255 is the colour.
 * @return The mixed colour.
 */
lv_color_t ui_tint(uint32_t color, uint8_t amount)
{
	return lv_color_mix(lv_color_hex(color), lv_color_hex(UI_SURFACE), amount);
}

/**
 * @brief Apply the full style set of one button variant to a button.
 *
 * @details Selects the fill, press, text, border, and font values for @p kind,
 *          then writes them for every state the widget can enter: default,
 *          pressed, focused, focus-key, checked, and disabled. The default theme
 *          adds focus and pressed outlines and uses recolor on press, so those
 *          properties are explicitly cleared for each state. A disabled button
 *          is finally dimmed to 70 % opacity.
 *
 * @param[in,out] button Button object to restyle.
 * @param[in]     kind   Visual variant to apply.
 */
void ui_style_button(lv_obj_t *button, enum ui_button_kind kind)
{
	uint32_t text = UI_TEXT;
	uint32_t border = UI_OUTLINE;
	lv_color_t background = lv_color_hex(UI_RAISED);
	lv_color_t pressed = lv_color_hex(UI_OUTLINE);
	lv_opa_t background_opa = LV_OPA_COVER;
	lv_border_side_t border_side = LV_BORDER_SIDE_FULL;
	lv_opa_t border_opa = LV_OPA_COVER;
	const lv_font_t *font = &cardsight_font_20;
	bool filled_press = true;

	/* pick the palette and geometry of the requested variant */
	switch (kind) {
	case UI_BUTTON_PRIMARY:
		background = lv_color_hex(UI_ACCENT);
		pressed = lv_color_white();
		text = UI_BACKGROUND;
		border = UI_ACCENT;
		break;
	case UI_BUTTON_DANGER:
		background = ui_tint(UI_DANGER, 70);
		pressed = ui_tint(UI_DANGER, 130);
		text = UI_DANGER;
		border = UI_DANGER;
		break;
	case UI_BUTTON_GHOST:
		background_opa = LV_OPA_TRANSP;
		pressed = lv_color_white();
		text = UI_ACCENT;
		border_side = LV_BORDER_SIDE_NONE;
		font = &cardsight_font_16;
		filled_press = false;
		break;
	default:
		/* list rows sit directly on the artwork: no fill, a hairline separator */
		background_opa = LV_OPA_TRANSP;
		pressed = lv_color_white();
		text = UI_TEXT;
		border = UI_OUTLINE;
		border_side = LV_BORDER_SIDE_BOTTOM;
		border_opa = 48;
		font = &cardsight_font_16;
		filled_press = false;
		break;
	}

	/*
	 * The default theme adds a focus/pressed outline and border of its own. Every property is
	 * written for every state we can enter, so nothing from the theme leaks through.
	 */
	{
		/** @brief Every widget state the button styling must cover. */
		static const lv_state_t states[] = {
			0, LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_CHECKED,
			LV_STATE_DISABLED,
		};

		for (uint32_t index = 0U; index < ARRAY_SIZE(states); index++) {
			lv_state_t state = states[index];
			bool is_pressed = state == LV_STATE_PRESSED;
			bool is_disabled = state == LV_STATE_DISABLED;

			/* fill: disabled wins, then the pressed colour, then the resting colour */
			lv_obj_set_style_bg_color(button,
				is_disabled ? lv_color_hex(UI_SURFACE) : is_pressed ? pressed : background, state);
			lv_obj_set_style_bg_opa(button,
				is_disabled ? LV_OPA_COVER :
				is_pressed ? (filled_press ? LV_OPA_COVER : LV_OPA_TRANSP) : background_opa,
				state);
			lv_obj_set_style_text_color(button,
				is_disabled ? lv_color_hex(UI_MUTED) :
				(is_pressed && kind == UI_BUTTON_PRIMARY) ? lv_color_hex(UI_BACKGROUND) :
				lv_color_hex(text), state);
			lv_obj_set_style_text_font(button, font, state);
			lv_obj_set_style_border_width(button, border_side == LV_BORDER_SIDE_NONE ? 0 : 1, state);
			lv_obj_set_style_border_side(button, border_side, state);
			lv_obj_set_style_border_color(button,
				is_disabled ? lv_color_hex(UI_OUTLINE) : lv_color_hex(border), state);
			lv_obj_set_style_border_opa(button, border_opa, state);
			/* clear the theme's focus ring, press shadow, and press recolor */
			lv_obj_set_style_outline_width(button, 0, state);
			lv_obj_set_style_outline_opa(button, LV_OPA_TRANSP, state);
			lv_obj_set_style_shadow_width(button, 0, state);
			lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, state);
			/* the default theme darkens the whole rectangle on press through recolor */
			lv_obj_set_style_recolor(button, lv_color_white(), state);
			lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, state);
			lv_obj_set_style_pad_all(button, 0, state);
			lv_obj_set_style_transform_width(button, 0, state);
			lv_obj_set_style_transform_height(button, 0, state);
		}
	}
	lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_DISABLED);
}

/**
 * @brief Style a plain card: background colour, radius, and padding.
 *
 * @details Fills the object with an opaque rounded background and removes the
 *          border and shadow so it reads as a flat surface.
 *
 * @param[in,out] object Card object to restyle.
 * @param[in]     color  Background colour, 0xRRGGBB.
 * @param[in]     radius Corner radius in pixels.
 */
void ui_style_card(lv_obj_t *object, uint32_t color, int32_t radius)
{
	lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(object, radius, 0);
	lv_obj_set_style_border_width(object, 0, 0);
	lv_obj_set_style_shadow_width(object, 0, 0);
}

/**
 * @brief Skin a scrollable object with the slim themed scrollbar.
 *
 * @details Styles LV_PART_SCROLLBAR: a four-pixel-wide, fully rounded bar in
 *          the muted colour, inset one pixel from the right edge.
 *
 * @param[in,out] object Scrollable object to restyle.
 */
void ui_style_scrollbar(lv_obj_t *object)
{
	lv_obj_set_style_width(object, 4, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(object, 2, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_color(object, lv_color_hex(UI_MUTED), LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(object, 1, LV_PART_SCROLLBAR);
}
