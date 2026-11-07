// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file main.c
 * @brief CardSight application entry point.
 *
 * Called by the board start-up code once the basic services are available. It
 * registers the board diagnostics first so a failure later can still be
 * inspected from the console, then starts the storage application task.
 */
#include "app/app.h"
#include "ui/ui.h"
#include "app/diagnostics.h"

/**
 * @brief Initialise the diagnostics and start the CardSight application.
 *
 * @details @c cardsight_diagnostics_init() installs the shell command and its
 * semaphore; @c cardsight_app_init() then creates the state lock, the request
 * queue and the worker task that owns all card access.
 */
void app_main(void)
{
	cardsight_diagnostics_init();
	cardsight_app_init();
}
