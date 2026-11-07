// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui.h
 * @brief Entry points of the CardSight touch UI.
 *
 * The UI runs as two RTOS tasks: a refresh task that posts redraw requests from
 * application state, and the LVGL runtime task that owns every widget.
 */
#ifndef CARDSIGHT_UI_H
#define CARDSIGHT_UI_H
/**
 * @brief Refresh task body: wait for change notifications and request redraws.
 *
 * @param[in] argument Unused task argument.
 */
void cardsight_ui_refresh(void *argument);
/**
 * @brief LVGL runtime task body: initialise the display and run the LVGL loop.
 *
 * @param[in] argument Unused task argument.
 */
void cardsight_ui_runtime(void *argument);
/**
 * @brief Create the diagnostics shell commands used to probe the UI.
 */
void cardsight_diagnostics_init(void);
/**
 * @brief Validate the LVGL object tree and report inconsistencies.
 *
 * @return 0 when the tree is consistent, or a negative errno on the first
 *         failure detected.
 */
int cardsight_validate_ui(void);
#endif
