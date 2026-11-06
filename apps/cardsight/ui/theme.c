// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/theme.h"

/* Mix a colour into the surface tone; amount 0 is the surface, 255 is the colour. */
lv_color_t ui_tint(uint32_t color, uint8_t amount)
{
	return lv_color_mix(lv_color_hex(color), lv_color_hex(UI_SURFACE), amount);
}

void ui_style_button(lv_obj_t *button, enum ui_button_kind kind)
{
	uint32_t text = UI_TEXT;
	uint32_t border = UI_OUTLINE;
	lv_color_t background = lv_color_hex(UI_RAISED);
	lv_color_t pressed = lv_color_hex(UI_OUTLINE);

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
		background = lv_color_hex(UI_SURFACE);
		pressed = lv_color_hex(UI_RAISED);
		text = UI_MUTED;
		border = UI_SURFACE;
		break;
	default:
		break;
	}
	lv_obj_set_style_bg_color(button, background, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(button, lv_color_hex(text), 0);
	lv_obj_set_style_text_font(button, &cardsight_font_20, 0);
	lv_obj_set_style_border_width(button, 1, 0);
	lv_obj_set_style_border_color(button, lv_color_hex(border), 0);
	lv_obj_set_style_outline_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_set_style_bg_color(button, pressed, LV_STATE_PRESSED);
	/* The stock theme grows a pressed button, which the screen edge and the parent clip. */
	lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
	lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
	if (kind == UI_BUTTON_PRIMARY) {
		lv_obj_set_style_text_color(button, lv_color_hex(UI_BACKGROUND), LV_STATE_PRESSED);
	}
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_SURFACE), LV_STATE_DISABLED);
	lv_obj_set_style_border_color(button, lv_color_hex(UI_OUTLINE), LV_STATE_DISABLED);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_MUTED), LV_STATE_DISABLED);
	lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_DISABLED);
}

void ui_style_card(lv_obj_t *object, uint32_t color, int32_t radius)
{
	lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(object, radius, 0);
	lv_obj_set_style_border_width(object, 0, 0);
	lv_obj_set_style_shadow_width(object, 0, 0);
}

void ui_style_scrollbar(lv_obj_t *object)
{
	lv_obj_set_style_width(object, 4, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(object, 2, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_color(object, lv_color_hex(UI_MUTED), LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(object, 1, LV_PART_SCROLLBAR);
}
