// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file widgets.h
 * @brief Small LVGL widget constructors and layout helpers shared by the pages.
 */
#ifndef CARDSIGHT_UI_WIDGETS_H
#define CARDSIGHT_UI_WIDGETS_H
#include <lvgl.h>
#include "ui/theme.h"
/**
 * @brief Format a sector count as a human-readable capacity string.
 *
 * @param[in]  sectors Capacity in 512-byte sectors.
 * @param[out] output  Buffer receiving the formatted text.
 * @param[in]  size    Size of @p output in bytes.
 */
void capacity_text(uint64_t sectors, char *output, size_t size);
/**
 * @brief Return the short display name of a partition content type.
 *
 * @param[in] content Partition content type.
 * @return Constant string such as "FAT32"; never NULL.
 */
const char *content_name(enum blk_partition_content content);
/**
 * @brief Return the chip colour used for a filesystem type name.
 *
 * @param[in] fstype Filesystem name as reported by the block layer.
 * @return ARGB colour for the filesystem chip.
 */
uint32_t fstype_color(const char *fstype);
/**
 * @brief Disable both scroll directions on an object.
 *
 * @param[in,out] object Object to make static.
 */
void disable_scroll(lv_obj_t *object);
/**
 * @brief Enable vertical scrolling with the themed elastic behaviour.
 *
 * @param[in,out] object Object that should scroll vertically.
 */
void enable_vertical_scroll(lv_obj_t *object);
/**
 * @brief Create a themed label as a child of @p parent.
 *
 * @param[in] parent Parent object.
 * @param[in] text   Initial label text.
 * @param[in] color  Text colour.
 * @return The new label object.
 */
lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color);
/*
 * Set label text, cutting it on a whole UTF-8 character when it is wider than max_width and
 * ending it with an ellipsis. A plain clipped label cuts the last glyph in half.
 */
/**
 * @brief Set label text, cutting it on a whole UTF-8 character with an ellipsis.
 *
 * @param[in,out] label     Label to update.
 * @param[in]     text      New text, possibly multi-byte UTF-8.
 * @param[in]     max_width Available width in pixels; the text is shortened
 *                          when it exceeds this width.
 */
void ui_label_set_ellipsis(lv_obj_t *label, const char *text, int32_t max_width);
/**
 * @brief Create a transparent layout box as a child of @p parent.
 *
 * @param[in] parent Parent object.
 * @param[in] width  Requested width in pixels.
 * @param[in] height Requested height in pixels.
 * @return The new box object.
 */
lv_obj_t *make_box(lv_obj_t *parent, int32_t width, int32_t height);
/**
 * @brief Create a styled card as a child of @p parent.
 *
 * @param[in] parent Parent object.
 * @param[in] width  Requested width in pixels.
 * @param[in] height Requested height in pixels.
 * @param[in] color  Background colour.
 * @return The new card object.
 */
lv_obj_t *make_card(lv_obj_t *parent, int32_t width, int32_t height, uint32_t color);
/* Lifted glass sheet with a light edge, specular rim and soft shadow. */
/**
 * @brief Create a lifted glass sheet with a light edge, specular rim and soft shadow.
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
	int32_t radius);
/**
 * @brief Create a small rounded status chip with centred text.
 *
 * @param[in] parent Parent object.
 * @param[in] text   Chip text.
 * @param[in] color  Chip accent colour.
 * @return The new chip object.
 */
lv_obj_t *make_chip(lv_obj_t *parent, const char *text, uint32_t color);
/**
 * @brief Create a neutral action button with a click callback.
 *
 * @param[in] parent   Parent object.
 * @param[in] text     Button label.
 * @param[in] callback Handler invoked on LV_EVENT_CLICKED.
 * @param[in] data     User data passed to @p callback.
 * @return The new button object.
 */
lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data);
/**
 * @brief Shared click handler that starts a busy UI operation.
 *
 * @param[in] event LVGL click event.
 */
void start_event(lv_event_t *event);
/**
 * @brief Create a fixed-width button for the bottom action bar.
 *
 * @param[in] parent   Parent object (the bottom bar).
 * @param[in] text     Button label.
 * @param[in] callback Handler invoked on LV_EVENT_CLICKED.
 * @param[in] width    Button width in pixels.
 * @return The new button object.
 */
lv_obj_t *make_bar_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, int32_t width);
/* Turn an existing action button into a glass pill with a coloured rim and text. */
/**
 * @brief Turn an existing action button into a glass pill with a coloured rim
 *        and text.
 *
 * @param[in,out] button Button to restyle.
 * @param[in]     color  Accent colour of the rim and label.
 */
void ui_glass_action(lv_obj_t *button, uint32_t color);
/* The neutral glass pill used by the bottom bar buttons. */
/**
 * @brief Apply the neutral glass pill look used by the bottom bar buttons.
 *
 * @param[in,out] button Button to restyle.
 */
void ui_glass_neutral(lv_obj_t *button);
/**
 * @brief Return the horizontal page margin derived from the screen radius.
 *
 * @return Margin in pixels.
 */
int32_t ui_margin(void);
/**
 * @brief Return the content width available on wide (landscape) layouts.
 *
 * @return Width in pixels.
 */
int32_t ui_wide_width(void);
/**
 * @brief Return the usable content height of the current page host.
 *
 * @return Height in pixels.
 */
int32_t ui_page_height(void);
/**
 * @brief Return the usable content width of the current page host.
 *
 * @return Width in pixels.
 */
int32_t ui_page_width(void);
/**
 * @brief Create a vertically scrolling list bounded by a top and bottom inset.
 *
 * @param[in] top    Distance from the page top in pixels.
 * @param[in] bottom Distance reserved below the list in pixels.
 * @return The new list object.
 */
lv_obj_t *make_list(int32_t top, int32_t bottom);
/**
 * @brief Enable or disable a widget and dim its label accordingly.
 *
 * @param[in,out] object   Widget to toggle.
 * @param[in]     disabled True to disable, false to enable.
 */
void set_disabled(lv_obj_t *object, bool disabled);
/* the wallpaper strips that fade the top and bottom of the scroll lists */
/**
 * @brief Forget the fade strips of the previous page.
 *
 * Called when a page is rebuilt so the refresh loop no longer touches stale
 * overlay objects.
 */
void ui_list_fades_reset(void);
/**
 * @brief Recompute the opacity of the fade strips from the current scroll
 *        position.
 */
void ui_refresh_list_fades(void);
/**
 * @brief Check that the fade strips match the current list state.
 *
 * @return 0 when the overlays are consistent, or a negative errno describing
 *         the first mismatch found.
 */
int ui_list_fades_check(void);

#endif
