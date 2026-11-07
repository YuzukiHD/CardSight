/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file panel-worker.h
 * @brief The panel's pixel pusher on hart 1 of the C907.
 *
 * Hart 0 renders into the draw buffer and hands a finished area over; hart 1 sends it over SPI
 * while hart 0 draws the next, so the wire and the renderer overlap. The cluster's cache
 * coherence unit is on, so the mailbox and the buffer need no explicit cache maintenance, only
 * fences.
 */

#ifndef CARDSIGHT_PANEL_WORKER_H
#define CARDSIGHT_PANEL_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Entry point of the worker on hart 1.
 *
 * @details Hart 1 enters here from the start code; it never returns.
 */
void sunxi_panel_worker_main(void) __attribute__((noreturn));

/**
 * @brief Hand a finished buffer over to the worker.
 *
 * @details Runs on hart 0. The buffer must stay valid and unchanged until the transfer reports
 * done, because the worker reads it from the other hart.
 *
 * @param[in] buffer Pixel bytes to send.
 * @param[in] length Size of @p buffer in bytes.
 * @return 0 on success, -EINVAL for an empty request, -ENODEV when the worker is not running,
 *         or -EBUSY when the previous one is still going.
 */
int sunxi_panel_worker_start(const void *buffer, size_t length);

/**
 * @brief Collect the result of the last hand-over.
 *
 * @details Runs on hart 0. Returns true once, with the result in @p result; the mailbox is
 * released back to idle.
 *
 * @param[out] result Result of the SPI transfer.
 * @return true when a transfer finished since the last call.
 */
bool sunxi_panel_worker_done(int *result);

/**
 * @brief Block until the worker is idle again.
 *
 * @details Runs on hart 0. The wire protocol needs the panel quiet, so a caller that is about
 * to send a command waits here first.
 */
void sunxi_panel_worker_wait(void);

/**
 * @brief Report the worker's state for the shell.
 *
 * @param[out] alive     Non-zero once the worker's loop has started.
 * @param[out] command   Current command word of the mailbox.
 * @param[out] heartbeat How often the worker's loop came around.
 */
void sunxi_panel_worker_status(uint32_t *alive, uint32_t *command, uint32_t *heartbeat);

#endif /* CARDSIGHT_PANEL_WORKER_H */
