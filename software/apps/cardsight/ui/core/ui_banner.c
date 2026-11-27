// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_banner.c
 * @brief The busy/progress banner.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/* Fill and text of the busy banner; called again for every progress step without a rebuild. */
/**
 * @brief Refresh the busy banner text and progress fill.
 *
 * @details Called again for every progress step without a rebuild. The label
 *          shows the operation and percentage when one is known; the fill is
 *          scaled to the bar width, kept at least two corner radii wide so it
 *          stays a pill, and hidden when progress is not positive.
 */
void ui_update_progress(void)
{
	char text[64];
	int percent = ui.model.progress;
	int32_t full = bar_width() - 2;

	if (ui.banner == NULL || ui.banner_fill == NULL || !ui.model.busy) {
		return;
	}
	if (percent >= 0) {
		snprintf(text, sizeof(text), "%s %d%%", ui.model.operation, percent);
	} else {
		snprintf(text, sizeof(text), "%s", ui.model.operation);
	}
	lv_label_set_text(ui.banner_label, text);
	lv_obj_set_width(ui.banner_fill, percent > 0 ? MAX(full * MIN(percent, 100) / 100, ui_bar_radius() * 2) : 0);
	lv_obj_set_hidden(ui.banner_fill, percent <= 0);
}

/* Progress while a request runs; failures get the error sheet instead. */
/**
 * @brief Build the progress banner that covers the bottom bar while busy.
 *
 * @details A tinted glass card with a translucent accent fill, a centred label,
 *          and a slide-in animation from behind the tab bar. Failures get the
 *          error sheet instead, so nothing is built when the request is idle.
 */
void build_banner(void)
{
	uint32_t color = UI_ACCENT;

	if (!ui.model.busy) {
		return;
	}
	ui.dismissed_error = 0;
	ui.banner = lv_obj_create(ui.screen);
	lv_obj_set_size(ui.banner, bar_width(), APP_BAR_HEIGHT);
	lv_obj_set_pos(ui.banner, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
	ui_style_card(ui.banner, UI_SURFACE, ui_bar_radius());
	lv_obj_set_style_bg_color(ui.banner, ui_tint(color, 60), 0);
	lv_obj_set_style_border_color(ui.banner, lv_color_hex(color), 0);
	lv_obj_set_style_border_width(ui.banner, 2, 0);
	lv_obj_set_style_pad_all(ui.banner, 0, 0);
	disable_scroll(ui.banner);
	ui.banner_fill = make_card(ui.banner, 0, APP_BAR_HEIGHT - 4, color);
	lv_obj_set_style_bg_opa(ui.banner_fill, LV_OPA_50, 0);
	lv_obj_set_style_radius(ui.banner_fill, ui_bar_radius() - 2, 0);
	lv_obj_set_pos(ui.banner_fill, 0, 0);
	ui.banner_label = make_label(ui.banner, "", UI_TEXT);
	lv_obj_set_style_text_font(ui.banner_label, &cardsight_font_20, 0);
	ui_update_progress();
	lv_obj_center(ui.banner_label);
	{
		/* slide in from behind the tab bar */
		int32_t resting = (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT;
		lv_anim_t animation;

		lv_obj_set_y(ui.banner, ui.animations ? resting + 56 : resting);
		lv_anim_init(&animation);
		lv_anim_set_var(&animation, ui.banner);
		lv_anim_set_exec_cb(&animation, (lv_anim_exec_xcb_t)lv_obj_set_y);
		lv_anim_set_values(&animation, resting + 56, resting);
		lv_anim_set_duration(&animation, ui.animations ? 220 : 1);
		lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
		lv_anim_start(&animation);
	}
}
