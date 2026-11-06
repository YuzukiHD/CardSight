/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel's pixel pusher on hart 1 of the C907. Hart 0 renders into the draw buffer and
 * hands a finished area over; hart 1 sends it over SPI while hart 0 draws the next, so the
 * wire and the renderer overlap. The cluster's cache coherence unit is on, so the mailbox and
 * the buffer need no explicit cache maintenance, only fences.
 */

#ifndef CARDSIGHT_PANEL_WORKER_H
#define CARDSIGHT_PANEL_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Hart 1 enters here from the start code; it never returns. */
void sunxi_panel_worker_main(void) __attribute__((noreturn));

/** Hart 0: hand @p buffer over to the worker; 0, -EBUSY when the previous one is still going. */
int sunxi_panel_worker_start(const void *buffer, size_t length);

/** Hart 0: the last hand-over has finished; true once, with the result in @p result. */
bool sunxi_panel_worker_done(int *result);

/** Hart 0: block until the worker is idle again (the wire protocol needs the panel quiet). */
void sunxi_panel_worker_wait(void);

/** Hart 0: for the shell: whether the worker runs and how often its loop came around. */
void sunxi_panel_worker_status(uint32_t *alive, uint32_t *command, uint32_t *heartbeat);

#endif /* CARDSIGHT_PANEL_WORKER_H */
