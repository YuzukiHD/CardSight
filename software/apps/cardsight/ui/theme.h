// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_UI_THEME_H
#define CARDSIGHT_UI_THEME_H
#include <lvgl.h>
#define UI_BACKGROUND 0x000000
#define UI_SURFACE 0x1c1f26
#define UI_RAISED 0x2e343f
#define UI_OUTLINE 0x5a6372
#define UI_TEXT 0xffffff
#define UI_MUTED 0xc6ccd8
#define UI_ACCENT 0x38bdf8
#define UI_SUCCESS 0x4ade80
#define UI_WARNING 0xfbbf24
#define UI_DANGER 0xff6b73
LV_FONT_DECLARE(cardsight_font_16);
LV_FONT_DECLARE(cardsight_font_20);
LV_FONT_DECLARE(cardsight_digits_32);
enum ui_button_kind {
	UI_BUTTON_NORMAL,
	UI_BUTTON_PRIMARY,
	UI_BUTTON_DANGER,
	UI_BUTTON_GHOST,
};
lv_color_t ui_tint(uint32_t color, uint8_t amount);
void ui_style_button(lv_obj_t *button, enum ui_button_kind kind);
void ui_style_card(lv_obj_t *object, uint32_t color, int32_t radius);
void ui_style_scrollbar(lv_obj_t *object);
#endif
