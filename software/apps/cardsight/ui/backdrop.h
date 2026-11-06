// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_UI_BACKDROP_H
#define CARDSIGHT_UI_BACKDROP_H

#include <lvgl.h>
#include <stdint.h>

/* The panel artwork as an LVGL image descriptor (RGB888). */
const lv_image_dsc_t *backdrop_image(void);
/* A one-time blurred copy of the artwork, for the frosted glass interior. */
const uint8_t *backdrop_blurred(void);
/* The same blurred copy as an image, used as the screen backdrop and by glass highlights. */
const lv_image_dsc_t *backdrop_blurred_image(void);

#endif
