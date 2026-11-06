/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LVGL on this board: the display flush, the touch screen as the input device, and a way for
 * other tasks to ask the LVGL task to run a function.
 */

#ifndef CARDSIGHT_LVGL_PORT_H
#define CARDSIGHT_LVGL_PORT_H

#include <stdbool.h>

#include <drm/sunxi_display.h>

typedef void (*lvgl_port_callback_t)(void *argument);

/** Run @p callback from the LVGL task (it may touch LVGL); 0, -ENOMEM or -EAGAIN. */
int lvgl_port_post(lvgl_port_callback_t callback, void *argument);

/** The display and the input device are up. */
bool lvgl_port_ready(void);

/** Wait, inside the LVGL task, until the panel has taken the last flush. */
void lvgl_port_wait_idle(void);

/** Check the guard word after the SRAM draw buffer; 0 when intact. */
int lvgl_port_check_buffer(void);

int lvgl_port_last_error(void);

#endif /* CARDSIGHT_LVGL_PORT_H */
