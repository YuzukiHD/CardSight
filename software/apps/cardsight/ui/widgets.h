// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_UI_WIDGETS_H
#define CARDSIGHT_UI_WIDGETS_H
#include <lvgl.h>
#include "ui/theme.h"
void capacity_text(uint64_t sectors, char *output, size_t size);
const char *content_name(enum blk_partition_content content);
uint32_t fstype_color(const char *fstype);
void disable_scroll(lv_obj_t *object);
void enable_vertical_scroll(lv_obj_t *object);
lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color);
lv_obj_t *make_box(lv_obj_t *parent, int32_t width, int32_t height);
lv_obj_t *make_card(lv_obj_t *parent, int32_t width, int32_t height, uint32_t color);
/* Lifted glass sheet with a light edge, specular rim and soft shadow. */
lv_obj_t *make_glass_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
	int32_t radius);
lv_obj_t *make_chip(lv_obj_t *parent, const char *text, uint32_t color);
lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data);
void start_event(lv_event_t *event);
lv_obj_t *make_bar_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, int32_t width);
/* Turn an existing action button into a glass pill with a coloured rim and text. */
void ui_glass_action(lv_obj_t *button, uint32_t color);
/* The neutral glass pill used by the bottom bar buttons. */
void ui_glass_neutral(lv_obj_t *button);
int32_t ui_margin(void);
int32_t ui_wide_width(void);
int32_t ui_page_height(void);
int32_t ui_page_width(void);
lv_obj_t *make_list(int32_t top, int32_t bottom);
void set_disabled(lv_obj_t *object, bool disabled);
#endif
