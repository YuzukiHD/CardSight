// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_UI_GLASS_H
#define CARDSIGHT_UI_GLASS_H

#include <lvgl.h>

/* Attach a refraction canvas to an already positioned, transparent widget. */
void glass_attach(lv_obj_t *object, int32_t radius);
/* Same, with an explicit tint strength (0..255) for content sheets. */
void glass_attach_tinted(lv_obj_t *object, int32_t radius, uint8_t opacity);
/* Sweep a bright glint along the top edge of a glass surface. */
void glass_glint(lv_obj_t *object);
/* A frosted-glass highlight sheet; returns the backdrop child to keep aligned when it moves. */
lv_obj_t *glass_highlight_attach(lv_obj_t *object, int32_t radius);
void glass_highlight_move(lv_obj_t *object, lv_obj_t *backdrop);

#endif
