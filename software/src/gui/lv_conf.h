/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LVGL on this board: the C library provides malloc, strings and formatting, and only the LVGL
 * task calls into LVGL, so there is no operating-system port to configure. Everything else is
 * left at the LVGL defaults.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS	      LV_OS_NONE

#define LV_COLOR_FORMAT_DEFAULT	 LV_COLOR_FORMAT_RGB565
#define LV_DEF_REFR_PERIOD	 33
#define LV_DPI_DEF		 130
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN	 64

#define LV_USE_LOG 0

#endif /* LV_CONF_H */
