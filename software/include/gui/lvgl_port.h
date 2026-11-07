/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file lvgl_port.h
 * @brief LVGL on this board: the display flush, the touch screen as the input device, and a way
 * for other tasks to ask the LVGL task to run a function.
 */

#ifndef CARDSIGHT_LVGL_PORT_H
#define CARDSIGHT_LVGL_PORT_H

#include <stdbool.h>

#include <drm/sunxi_display.h>

/** @brief Function another task asks the LVGL task to run. */
typedef void (*lvgl_port_callback_t)(void *argument);

/**
 * @brief Run @p callback from the LVGL task.
 *
 * @details The request is queued without waiting; the LVGL task drains the queue on every
 * iteration, so the callback may touch LVGL.
 *
 * @param callback Function to run; must not be NULL.
 * @param argument Opaque value passed to @p callback.
 * @return 0 when queued, -EINVAL for a NULL callback, -EAGAIN before the port is up, or
 * -ETIMEDOUT when the request queue is full.
 */
int lvgl_port_post(lvgl_port_callback_t callback, void *argument);

/**
 * @brief Whether the display and the input device are up.
 *
 * @return true once both devices are ready.
 */
bool lvgl_port_ready(void);

/**
 * @brief Wait, inside the LVGL task, until the panel has taken the last flush.
 *
 * @details Sleeps in short steps while the asynchronous write is still in flight.
 */
void lvgl_port_wait_idle(void);

/**
 * @brief Check the guard word after the SRAM draw buffer.
 *
 * @return 0 when intact, or -1 (with a log message) when the guard word was overwritten.
 */
int lvgl_port_check_buffer(void);

/**
 * @brief The last error recorded by the LVGL task.
 *
 * @return The error, or 0 when there was none.
 */
int lvgl_port_last_error(void);

#endif /* CARDSIGHT_LVGL_PORT_H */
