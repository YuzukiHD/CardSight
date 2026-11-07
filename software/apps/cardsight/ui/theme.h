// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file theme.h
 * @brief Colour palette, fonts, and shared style helpers of the CardSight UI.
 *
 * The palette comes from the liquid-glass design tokens: a quiet navy content
 * layer with a bright ice accent, plus the translucent glass control colours.
 */
#ifndef CARDSIGHT_UI_THEME_H
#define CARDSIGHT_UI_THEME_H
#include <lvgl.h>
/** @brief Screen background colour; deepest navy layer. */
#define UI_BACKGROUND 0x071421
/** @brief Default surface colour for content cards. */
#define UI_SURFACE 0x0d1b28
/** @brief Raised surface colour for cards floating above the content layer. */
#define UI_RAISED 0x1b2c3b
/** @brief Outline and separator colour. */
#define UI_OUTLINE 0x526b7d
/** @brief Primary text colour. */
#define UI_TEXT 0xf5f8fb
/** @brief Secondary, de-emphasised text colour. */
#define UI_MUTED 0xaabac8
/** @brief Accent colour for interactive and selected elements. */
#define UI_ACCENT 0x9fe4ff
/** @brief Colour for success status text and chips. */
#define UI_SUCCESS 0x83e7b8
/** @brief Colour for warning status text and chips. */
#define UI_WARNING 0xffd37a
/** @brief Colour for destructive actions and error status. */
#define UI_DANGER 0xff949a
/** @brief Glass surfaces: the reference's control tint over the blurred artwork. */
#define UI_GLASS_TINT 0x526b7d
/** @brief Opacity (0..255) applied to UI_GLASS_TINT on glass surfaces. */
#define UI_GLASS_TINT_OPA 154
/** @brief Glass surfaces: colour of the specular rim around a glass edge. */
#define UI_GLASS_EDGE 0xffffff
/** @brief Opacity (0..255) of the glass rim highlight. */
#define UI_GLASS_EDGE_OPA 40
/** @brief Declares the 16 px body font generated for the UI. */
LV_FONT_DECLARE(cardsight_font_16);
/** @brief Declares the 20 px heading font generated for the UI. */
LV_FONT_DECLARE(cardsight_font_20);
/** @brief Declares the 32 px tabular digit font used for counters and rates. */
LV_FONT_DECLARE(cardsight_digits_32);
/**
 * @brief Visual variants accepted by ui_style_button().
 */
enum ui_button_kind {
	/** @brief Neutral secondary button on a raised surface. */
	UI_BUTTON_NORMAL,
	/** @brief Filled accent button for the primary action of a page. */
	UI_BUTTON_PRIMARY,
	/** @brief Filled danger button for destructive actions. */
	UI_BUTTON_DANGER,
	/** @brief Transparent button with only a text and rim treatment. */
	UI_BUTTON_GHOST,
};
/**
 * @brief Blend a 24-bit RGB colour towards white by a fixed amount.
 *
 * @param[in] color  Source colour in 0xRRGGBB form.
 * @param[in] amount Blend weight towards white, 0..255.
 * @return The tinted colour.
 */
lv_color_t ui_tint(uint32_t color, uint8_t amount);
/**
 * @brief Apply the full style set of one button variant to a button.
 *
 * @param[in,out] button Button object to restyle.
 * @param[in]     kind   Visual variant to apply.
 */
void ui_style_button(lv_obj_t *button, enum ui_button_kind kind);
/**
 * @brief Style a plain card: background colour, radius, and padding.
 *
 * @param[in,out] object Card object to restyle.
 * @param[in]     color  Background colour.
 * @param[in]     radius Corner radius in pixels.
 */
void ui_style_card(lv_obj_t *object, uint32_t color, int32_t radius);
/**
 * @brief Skin a scrollable object with the slim themed scrollbar.
 *
 * @param[in,out] object Scrollable object to restyle.
 */
void ui_style_scrollbar(lv_obj_t *object);
#endif
