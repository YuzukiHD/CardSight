// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file icons.h
 * @brief Declarations of the RGB565 icon images used by the UI.
 *
 * The image data itself lives in the generated asset sources; this header only
 * publishes the LVGL image descriptors so pages and tabs can reference them.
 */
#ifndef CARDSIGHT_UI_ICONS_H
#define CARDSIGHT_UI_ICONS_H
#include <lvgl.h>

/** @brief USB mass-storage icon shown on the Direct page and tab bar. */
LV_IMAGE_DECLARE(cardsight_icon_usb);
/** @brief Storage proxy icon shown on the Proxy page and tab bar. */
LV_IMAGE_DECLARE(cardsight_icon_proxy);
/** @brief Drive icon used for whole-card partitions. */
LV_IMAGE_DECLARE(cardsight_icon_drive);
/** @brief Folder icon used for browsable directory rows. */
LV_IMAGE_DECLARE(cardsight_icon_folder);
/** @brief Information icon for the Info page. */
LV_IMAGE_DECLARE(cardsight_icon_info);
/** @brief Settings gear icon for the Manage page. */
LV_IMAGE_DECLARE(cardsight_icon_gear);

#endif
