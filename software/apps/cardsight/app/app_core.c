// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file app_core.c
 * @brief Application state, UI notifications, initialisation and snapshots.
 *
 * @details Part of the CardSight application. The shared model is declared in
 * @c app/app_private.h; the UI is refreshed through the LVGL port.
 */
#include "app/app_private.h"

/** @brief The state every module reads; protected by @c app_lock. */
struct cardsight_state app;
/** @brief Lock guarding @c app. */
struct mutex app_lock;
/** @brief Queue the UI sends requests through; the worker drains it. */
struct message_queue requests;



/**
 * @brief Request one UI refresh without waiting for the LVGL task.
 *
 * @details The post is best-effort: a full LVGL queue drops this refresh,
 * which is acceptable because the caller posts another on the next event.
 */
void app_post_refresh(void)
{
	(void)lvgl_port_post(cardsight_ui_refresh, NULL);
}


/**
 * @brief Request a UI refresh, retrying until the LVGL queue accepts it.
 *
 * @details The last refresh of a request must not be lost to a full queue, or
 * the banner never clears. The function retries once every 10 ms for up to one
 * second and then gives up rather than block the worker forever.
 */
void app_post_refresh_final(void)
{
	for (uint32_t attempt = 0U; attempt < 100U; attempt++) {
		if (lvgl_port_post(cardsight_ui_refresh, NULL) == 0) {
			return;
		}
		msleep(10);
	}
}

/**
 * @brief Post a refresh of the live runtime values to the LVGL task.
 */
void app_post_refresh_runtime(void)
{
	(void)lvgl_port_post(cardsight_ui_runtime, NULL);
}


/**
 * @brief Create the state lock, the request queue and the worker task.
 *
 * @details A failure to create the lock or the queue leaves the application
 * uninitialised and is logged; a failure to create the task is logged only,
 * because the shell still works without the worker.
 */
void cardsight_app_init(void)
{
	memset(&app, 0, sizeof(app));
	app.progress = -1;
	mutex_init(&app_lock);
	if (app_lock.handle == NULL || msg_queue_init(&requests, sizeof(struct cardsight_request), 6U) != 0) {
		pr_err("cannot initialize CardSight storage app\n");
		return;
	}
	if (xTaskCreate(worker, "cardsight", 8192U, NULL, 2U, NULL) != pdPASS) {
		pr_err("cannot create CardSight storage task\n");
	}
}


/**
 * @brief Copy the shared state for the UI to render.
 *
 * @details The copy is made under @c app_lock, then the USB running flag is
 * refreshed outside the lock because it is owned by the USB driver.
 *
 * @param[out] state Snapshot destination.
 */
void cardsight_snapshot(struct cardsight_state *state)
{
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	*state = app;
	state->running = usb_mass_storage_running();
	mutex_unlock(&app_lock);
}
