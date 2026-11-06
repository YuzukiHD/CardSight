// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_UI_THEME_H
#define CARDSIGHT_UI_THEME_H
#include <lvgl.h>
/* Palette from the liquid-glass design tokens: quiet navy content, bright ice accent. */
#define UI_BACKGROUND 0x071421
#define UI_SURFACE 0x0d1b28
#define UI_RAISED 0x1b2c3b
#define UI_OUTLINE 0x526b7d
#define UI_TEXT 0xf5f8fb
#define UI_MUTED 0xaabac8
#define UI_ACCENT 0x9fe4ff
#define UI_SUCCESS 0x83e7b8
#define UI_WARNING 0xffd37a
#define UI_DANGER 0xff949a
/* Glass surfaces: the reference's control tint over the blurred artwork, with ring optics. */
#define UI_GLASS_TINT 0x526b7d
#define UI_GLASS_TINT_OPA 154
#define UI_GLASS_EDGE 0xffffff
#define UI_GLASS_EDGE_OPA 40
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
