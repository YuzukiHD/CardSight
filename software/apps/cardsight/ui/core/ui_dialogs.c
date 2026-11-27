// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_dialogs.c
 * @brief Touch calibration, settings sheet and the error sheet.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/* Rounded outline of the glass and the bar inset, to line up against the real corners. */
/**
 * @brief Click handler for the radius calibration overlay.
 *
 * @details Reads the touch point: the left third shrinks the radius, the right
 *          third grows it, and the middle accepts it, logs the value, closes
 *          the overlay, and schedules a rebuild.
 *
 * @param[in] event LVGL click event; the active input device supplies the point.
 */
void calibration_event(lv_event_t *event)
{
	lv_indev_t *input = lv_indev_active();
	lv_point_t point = { 0 };
	int32_t step = 0;

	(void)event;
	if (input != NULL) {
		lv_indev_get_point(input, &point);
	}
	if (point.x < (int32_t)ui.width / 3) {
		step = -1;
	} else if (point.x >= (int32_t)ui.width * 2 / 3) {
		step = 1;
	}
	if (step == 0) {
		pr_info("CardSight screen radius %ld\n", (long)ui.screen_radius);
		lv_obj_delete_async(ui.calibration);
		ui.calibration = NULL;
		ui_schedule_rebuild();
		return;
	}
	ui_open_calibration(ui.screen_radius + step);
}

/* Left third shrinks the radius, right third grows it, the middle accepts it. */
/**
 * @brief Open the touch calibration overlay with a given screen radius.
 *
 * @details Clamps the radius to 8..60 px, replaces any existing overlay with a
 *          full-screen card whose border marks the glass outline, and draws the
 *          current value plus the gesture hints. An inner box shows where the
 *          bar inset lands relative to the rounded corner.
 *
 * @param[in] radius Requested corner radius in pixels.
 */
void ui_open_calibration(int32_t radius)
{
	lv_obj_t *inner;
	lv_obj_t *label;
	char text[48];

	ui.screen_radius = CLAMP(radius, 8, 60);
	if (ui.calibration != NULL) {
		lv_obj_delete_async(ui.calibration);
	}
	ui.calibration = lv_obj_create(ui.screen);
	lv_obj_set_size(ui.calibration, ui.width, ui.height);
	lv_obj_set_pos(ui.calibration, 0, 0);
	ui_style_card(ui.calibration, UI_BACKGROUND, ui.screen_radius);
	lv_obj_set_style_border_width(ui.calibration, 3, 0);
	lv_obj_set_style_border_color(ui.calibration, lv_color_hex(UI_ACCENT), 0);
	lv_obj_set_style_pad_all(ui.calibration, 0, 0);
	disable_scroll(ui.calibration);
	lv_obj_add_event_cb(ui.calibration, calibration_event, LV_EVENT_CLICKED, NULL);
	inner = make_box(ui.calibration, bar_width(), APP_BAR_HEIGHT);
	lv_obj_set_style_border_width(inner, 1, 0);
	lv_obj_set_style_border_color(inner, lv_color_hex(UI_WARNING), 0);
	lv_obj_set_style_radius(inner, ui_bar_radius(), 0);
	lv_obj_set_pos(inner, APP_BAR_MARGIN - 3, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT - 3);
	snprintf(text, sizeof(text), "Screen corner %ld px", (long)ui.screen_radius);
	label = make_label(ui.calibration, text, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -14);
	label = make_label(ui.calibration, "Left/right adjust, mid done", UI_MUTED);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 18);
}

/**
 * @brief Click handler that closes the settings sheet.
 *
 * @param[in] event LVGL click event; not inspected.
 */
void settings_close_event(lv_event_t *event)
{
	(void)event;
	if (ui.settings_dialog != NULL) {
		lv_obj_delete_async(ui.settings_dialog);
		ui.settings_dialog = NULL;
	}
}

/* The sheet is built outside the input event: creating this many objects while the
 * button is being released made LVGL walk a stale style list and fault. */
/**
 * @brief Deferred builder for the settings sheet.
 *
 * @details The sheet is built outside the input event: creating this many
 *          objects while the button was being released made LVGL walk a stale
 *          style list and fault. It lays out a full-screen background, a title,
 *          the product, author, and licence lines in a column, and a bottom back
 *          button.
 *
 * @param[in] argument Unused async-call argument.
 */
void settings_open_async(void *argument)
{
	lv_obj_t *dialog;
	lv_obj_t *list;
	lv_obj_t *button;
	lv_obj_t *label;

	(void)argument;
	if (ui.settings_dialog != NULL) {
		return;
	}
	dialog = lv_obj_create(ui.screen);
	ui.settings_dialog = dialog;
	lv_obj_set_size(dialog, ui.width, ui.height);
	lv_obj_set_pos(dialog, 0, 0);
	ui_style_card(dialog, UI_BACKGROUND, 0);
	lv_obj_set_style_pad_all(dialog, 0, 0);
	disable_scroll(dialog);
	label = make_label(dialog, "Settings", UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_set_pos(label, ui_margin() + 4, ui_margin());
	list = lv_obj_create(dialog);
	lv_obj_set_size(list, ui.width,
		(int32_t)ui.height - APP_HEADER_HEIGHT - APP_BAR_HEIGHT - 2 * ui_margin());
	lv_obj_set_pos(list, 0, APP_HEADER_HEIGHT + ui_margin());
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_outline_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_hor(list, APP_EDGE_PAD, 0);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	label = make_label(list, "CardSight", UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	make_label(list, "YuzukiTsuru", UI_ACCENT);
	make_label(list, "GPL-2.0-or-later", UI_MUTED);
	button = make_bar_button(dialog, "Back", settings_close_event, (int32_t)ui.width - 2 * APP_BAR_MARGIN);
	lv_obj_set_pos(button, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
}

/**
 * @brief Click handler that opens the settings sheet asynchronously.
 *
 * @param[in] event LVGL click event; not inspected.
 */
void settings_event(lv_event_t *event)
{
	(void)event;
	(void)lv_async_call(settings_open_async, NULL);
}

/**
 * @brief Return the heading for an error sheet from the failed request type.
 *
 * @param[in] request Request that failed.
 * @return Constant heading; never NULL.
 */
const char *error_title(enum cardsight_request_type request)
{
	switch (request) {
	case CARDSIGHT_REQUEST_SCAN:
	case CARDSIGHT_REQUEST_LIST:
		return "Read failed";
	case CARDSIGHT_REQUEST_FORMAT:
	case CARDSIGHT_REQUEST_LAYOUT:
		return "Format failed";
	case CARDSIGHT_REQUEST_START_DIRECT:
	case CARDSIGHT_REQUEST_START_PROXY:
		return "Connect failed";
	case CARDSIGHT_REQUEST_STOP:
		return "Stop failed";
	default:
		return "Operation failed";
	}
}

/**
 * @brief Click handler that dismisses the error sheet.
 *
 * @details Records the dismissed error code so the refresh does not reopen the
 *          same sheet, and deletes the dialog asynchronously.
 *
 * @param[in] event LVGL click event; not inspected.
 */
void error_close_event(lv_event_t *event)
{
	(void)event;
	ui.dismissed_error = ui.model.error;
	if (ui.error_dialog != NULL) {
		lv_obj_delete_async(ui.error_dialog);
		ui.error_dialog = NULL;
	ui.detecting_card = NULL;
	}
}

/* Same material as the settings sheet: a full-screen frost, a title, one glass card with the
 * concrete failure, and the bar button at the bottom. Created outside the input event. */
/**
 * @brief Deferred builder for the error sheet.
 *
 * @details Uses the same material as the settings sheet: a full-screen frost, a
 *          title, one glass card with the operation name, the controller's
 *          verbatim failure, the error code, and the path when listing failed,
 *          plus the bottom back button. Created outside the input event.
 *
 * @param[in] argument Unused async-call argument.
 */
void error_open_async(void *argument)
{
	lv_obj_t *dialog;
	lv_obj_t *card;
	lv_obj_t *label;
	lv_obj_t *button;
	char text[64];

	(void)argument;
	if (ui.error_dialog != NULL || ui.model.error == 0 || ui.model.busy) {
		return;
	}
	dialog = lv_obj_create(ui.screen);
	ui.error_dialog = dialog;
	lv_obj_set_size(dialog, ui.width, ui.height);
	lv_obj_set_pos(dialog, 0, 0);
	ui_style_card(dialog, UI_BACKGROUND, 0);
	lv_obj_set_style_pad_all(dialog, 0, 0);
	disable_scroll(dialog);
	label = make_label(dialog, error_title(ui.model.request), UI_DANGER);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_set_pos(label, ui_margin() + 4, ui_margin());
	card = make_glass_card(dialog, ui_margin(), APP_HEADER_HEIGHT + ui_margin(),
		ui_wide_width(), 148, APP_CARD_RADIUS);
	label = make_label(card, ui.model.operation[0] != '\0' ? ui.model.operation : "Operation failed",
		UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_set_pos(label, 14, 10);
	{
		int32_t y = 38;

		if (ui.model.card_error[0] != '\0') {
			/* the controller's verbatim failure, e.g. "CMD5 Read Timeout" */
			label = make_label(card, ui.model.card_error, UI_WARNING);
			lv_obj_set_width(label, ui_wide_width() - 28);
			lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
			ui_label_set_ellipsis(label, ui.model.card_error, ui_wide_width() - 28);
			lv_obj_set_pos(label, 14, y);
			y += 24;
		}
		snprintf(text, sizeof(text), "Error code: %d", ui.model.error);
		label = make_label(card, text, UI_MUTED);
		lv_obj_set_pos(label, 14, y);
		y += 24;
		if (ui.model.request == CARDSIGHT_REQUEST_LIST && ui.model.files.path[0] != '\0') {
			label = make_label(card, ui.model.files.path, UI_MUTED);
			lv_obj_set_width(label, ui_wide_width() - 28);
			lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
			ui_label_set_ellipsis(label, ui.model.files.path, ui_wide_width() - 28);
			lv_obj_set_pos(label, 14, y);
			y += 24;
		}
		label = make_label(card, "Check the card and retry", UI_MUTED);
		lv_obj_set_pos(label, 14, y);
	}
	button = make_bar_button(dialog, "Back", error_close_event,
		(int32_t)ui.width - 2 * APP_BAR_MARGIN);
	lv_obj_set_pos(button, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
}

/* A failed request keeps its error in the snapshot until the sheet is dismissed. */
/**
 * @brief Open the error sheet when a request failed and was not dismissed.
 *
 * @details A new request clears the dismissed marker; a zero or already
 *          dismissed error, or an open sheet, suppresses the popup.
 */
void ui_error_check(void)
{
	if (ui.model.busy) {
		ui.dismissed_error = 0; /* a new request earns a new sheet */
		return;
	}
	if (ui.model.error == 0 || ui.model.error == ui.dismissed_error ||
		ui.error_dialog != NULL) {
		return;
	}
	(void)lv_async_call(error_open_async, NULL);
}
