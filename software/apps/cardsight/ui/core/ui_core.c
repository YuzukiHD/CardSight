// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_core.c
 * @brief The live UI instance, the deferred rebuild path and the LVGL runtime task.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/** @brief The single live UI instance; only the LVGL task mutates it. */
struct cardsight_ui ui;

/**
 * @brief Drop every cached widget pointer before a rebuild.
 *
 * @details The screen is about to be cleaned, so all pointers into the old tree
 *          become stale. The tab arrays are zeroed as well; pages refill their
 *          own pointers when they are rebuilt.
 */
void ui_clear_page_refs(void)
{
	ui.tabbar = NULL;
	ui.tab_indicator = NULL;
	ui.tab_indicator_backdrop = NULL;
	ui.header_title = NULL;
	ui.header_count = NULL;
	ui.header_status = NULL;
	ui.background = NULL;
	ui.bar = NULL;
	ui.layout_apply = NULL;
	ui.calibration = NULL;
	memset(ui.tab_buttons, 0, sizeof(ui.tab_buttons));
	memset(ui.tab_icons, 0, sizeof(ui.tab_icons));
	ui.banner = NULL;
	ui.banner_fill = NULL;
	ui.banner_label = NULL;
	ui.format_dialog = NULL;
	ui.settings_dialog = NULL;
	ui.error_dialog = NULL;
	ui.detecting_card = NULL;
	ui.direct_status = NULL;
	ui.direct_status_chip = NULL;
	ui.rate_read = NULL;
	ui.rate_write = NULL;
	ui.proxy_start = NULL;
}

/**
 * @brief LVGL async callback that performs a deferred rebuild.
 *
 * @details Clears the pending flag first, so a rebuild triggered from inside
 *          the refresh can schedule another one.
 *
 * @param[in] argument Unused async-call argument.
 */
void ui_async_rebuild(void *argument)
{
	(void)argument;
	ui.rebuild_pending = false;
	cardsight_ui_refresh(NULL);
}

/**
 * @brief Request a deferred rebuild of the whole widget tree.
 *
 * @details Coalesces multiple requests: while one async call is pending no
 *          second one is queued. The rebuild then runs on the LVGL task.
 */
void ui_schedule_rebuild(void)
{
	if (!ui.rebuild_pending) {
		ui.rebuild_pending = lv_async_call(ui_async_rebuild, NULL) == LV_RESULT_OK;
	}
}

/**
 * @brief Find the scrollable list inside a page tile.
 *
 * @details Returns the first direct child of the tile that LVGL reports as
 *          scrollable, which is the list built by make_list().
 *
 * @param[in] index Tile index.
 * @return The list object, or NULL when the tile has none.
 */
lv_obj_t *tile_list(uint32_t index)
{
	lv_obj_t *tile = ui.tiles[index];

	for (uint32_t child = 0U; tile != NULL && child < lv_obj_get_child_count(tile); child++) {
		lv_obj_t *item = lv_obj_get_child(tile, (int32_t)child);

		if (lv_obj_is_scrollable(item)) {
			return item;
		}
	}
	return NULL;
}

/**
 * @brief Rebuild the whole UI from the current application snapshot.
 *
 * @details Takes a model snapshot, keeps the Files detail state consistent with
 *          card presence, initialises the partition selection once, and resets
 *          the view state when the card changed. It then reads the display
 *          geometry, creates the root screen on first use, saves every list's
 *          scroll position, cleans the tree, and rebuilds the wallpaper, shell,
 *          header, settings button, page tiles, bottom bar, and the detection
 *          spinner. Finally it restores the visible tile, scroll positions, an
 *          open format dialog, the busy banner, and the error sheet.
 *
 * @param[in] argument Unused task argument.
 */
void cardsight_ui_refresh(void *argument)
{
	int32_t scroll[APP_TAB_COUNT] = { 0 };

	struct sunxi_display_info display_info;

	(void)argument;
	cardsight_snapshot(&ui.model);
	/* a missing card cannot stay in the Files detail view */
	if (!ui.model.card_present) {
		ui.files_detail = false;
	}
	/* initial selection: every partition that exists */
	if (!ui.selection_initialized && ui.model.partition_count != 0U) {
		for (size_t index = 0; index < ui.model.partition_count; index++)
			ui.selected |= 1U << (ui.model.partitions[index].partno - 1U);
		ui.selection_initialized = true;
	}
	/* a different size means a different card; reset the per-card view state */
	if (ui.model.card_present && (ui.layout_count == 0U || ui.layout_sectors != ui.model.sectors)) {
		/* A different card: its size, partitions and selection are not the old card's. */
		default_layout();
		ui.layout_table = ui.model.table;
		if (ui.layout_sectors != 0U) {
			ui.selected = 0U;
			ui.selection_initialized = false;
			ui.layout_editor = false;
			ui.manage_detail = false;
			ui.files_detail = false;
		}
		ui.layout_sectors = ui.model.sectors;
	}
	/* read the panel geometry so the layout follows rotation */
	if (sunxi_display_get_info(&display_info) == 0) {
		ui.width = display_info.width;
		ui.height = display_info.height;
		ui.landscape = ui.width >= ui.height;
	}
	/* first use: configure the root screen once */
	if (ui.screen == NULL) {
		ui.animations = true;
		ui.screen = lv_screen_active();
		lv_obj_set_style_bg_color(ui.screen, lv_color_hex(UI_BACKGROUND), 0);
		lv_obj_set_style_text_font(ui.screen, &cardsight_font_16, 0);
		lv_obj_set_style_pad_all(ui.screen, 0, 0);
		disable_scroll(ui.screen);
	}
	/* A rebuild must not throw a list back to the top while the finger is moving it. */
	for (uint32_t index = 0U; index < APP_TAB_COUNT; index++) {
		lv_obj_t *list = tile_list(index);

		scroll[index] = list != NULL ? lv_obj_get_scroll_y(list) : 0;
	}
	/* drop the old tree and every cached pointer into it */
	lv_obj_clean(ui.screen);
	ui_list_fades_reset();
	ui.shell = NULL;
	ui.page_host = NULL;
	memset(ui.tiles, 0, sizeof(ui.tiles));
	ui_clear_page_refs();
	if (ui.screen_radius == 0) {
		ui.screen_radius = APP_SCREEN_RADIUS;
	}
	/* wallpaper first so every glass surface can sample it */
	{
		ui.background = lv_image_create(ui.screen);
		lv_image_set_src(ui.background, backdrop_image());
		lv_obj_set_pos(ui.background, 0, 0);
	}
	ui.shell = lv_obj_create(ui.screen);
	lv_obj_set_size(ui.shell, ui.width, ui.height);
	lv_obj_set_pos(ui.shell, 0, 0);
	lv_obj_set_style_bg_opa(ui.shell, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(ui.shell, 0, 0);
	lv_obj_set_style_radius(ui.shell, 0, 0);
	lv_obj_set_style_pad_all(ui.shell, 0, 0);
	disable_scroll(ui.shell);

	ui.page_host = lv_obj_create(ui.shell);
	lv_obj_set_size(ui.page_host, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(ui.page_host, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(ui.page_host, 0, 0);
	lv_obj_set_style_pad_all(ui.page_host, 0, 0);
	disable_scroll(ui.page_host);
	ui.header_title = make_label(ui.shell, "", UI_TEXT);
	lv_obj_set_style_text_font(ui.header_title, &cardsight_font_20, 0);
	lv_obj_set_pos(ui.header_title, ui_margin(), ui_margin() - 2);
	ui.header_count = make_label(ui.shell, "", UI_MUTED);
	lv_obj_set_style_text_font(ui.header_count, &cardsight_font_16, 0);
	lv_obj_align(ui.header_count, LV_ALIGN_TOP_MID, 0, ui_margin() + 2);
	ui.header_status = make_label(ui.shell, "", UI_MUTED);
	lv_obj_set_style_text_font(ui.header_status, &cardsight_font_16, 0);
	lv_obj_align(ui.header_status, LV_ALIGN_TOP_RIGHT, -ui_margin() - 38, ui_margin() + 2);
	{
		/* the settings button, a small round glass like the selected tab */
		lv_obj_t *settings = make_glass_card(ui.shell,
			(int32_t)ui.width - ui_margin() - 30, ui_margin() - 3, 30, 30, 15);
		lv_obj_t *glyph = lv_image_create(settings);

		lv_image_set_src(glyph, &cardsight_icon_gear);
		lv_obj_set_style_image_recolor(glyph, lv_color_hex(UI_MUTED), 0);
		lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
		lv_image_set_scale(glyph, 200U);
		lv_obj_center(glyph);
		lv_obj_set_clickable(settings, true);
		lv_obj_add_event_cb(settings, settings_event, LV_EVENT_CLICKED, NULL);
	}
	/* one tile per page, all built up front */
	for (uint32_t index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		ui.tiles[index] = lv_obj_create(ui.page_host);
		lv_obj_set_size(ui.tiles[index], lv_pct(100), lv_pct(100));
		lv_obj_set_pos(ui.tiles[index], 0, 0);
		lv_obj_set_style_bg_opa(ui.tiles[index], LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(ui.tiles[index], 0, 0);
		/* The tile spans the glass so a scrolling list can run to the edge; the padding keeps the
		 * content box where the pages lay out their fixed controls. */
		lv_obj_set_style_pad_all(ui.tiles[index], ui_margin(), 0);
		lv_obj_set_style_pad_top(ui.tiles[index], ui_margin() + APP_HEADER_HEIGHT, 0);
		lv_obj_set_style_pad_bottom(ui.tiles[index],
			(int32_t)ui.height - ui_margin() - APP_HEADER_HEIGHT - ui_page_height(), 0);
		disable_scroll(ui.tiles[index]);
	}
	ui.content = ui.tiles[CARDSIGHT_DIRECT];
	build_direct();
	ui.content = ui.tiles[CARDSIGHT_PROXY];
	build_proxy();
	ui.content = ui.tiles[CARDSIGHT_MANAGE];
	build_manage();
	ui.content = ui.tiles[CARDSIGHT_FILES];
	build_files();
	ui.content = ui.tiles[CARDSIGHT_INFO];
	build_info();
	build_bar();
	{
		/* A card is being identified: a small spinner over the current page. */
		lv_obj_t *spinner;
		lv_obj_t *label;

		ui.detecting_card = make_glass_card(ui.shell, 0, 0, 170, 52, 26);
		lv_obj_align(ui.detecting_card, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_clickable(ui.detecting_card, false);
		spinner = lv_spinner_create(ui.detecting_card);
		lv_obj_set_size(spinner, 24, 24);
		lv_spinner_set_anim_params(spinner, 900, 60);
		lv_obj_align(spinner, LV_ALIGN_LEFT_MID, 16, 0);
		lv_obj_set_style_arc_color(spinner, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
		label = make_label(ui.detecting_card, "正在识别卡", UI_TEXT);
		lv_obj_align(label, LV_ALIGN_LEFT_MID, 52, 0);
		lv_obj_set_hidden(ui.detecting_card, true);
	}
	/* restore the page the user was looking at */
	ui.content = ui.tiles[(uint32_t)ui.mode];
	for (uint32_t index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		lv_obj_set_hidden(ui.tiles[index], index != (uint32_t)ui.mode);
	}
	ui_update_tabs();
	ui_update_header();
	ui_update_detecting();
	ui_update_backdrop();
	lv_obj_update_layout(ui.screen);
	ui_refresh_list_fades();
	for (uint32_t index = 0U; index < APP_TAB_COUNT; index++) {
		lv_obj_t *list = tile_list(index);

		if (list != NULL && scroll[index] != 0) {
			lv_obj_scroll_to_y(list, scroll[index], LV_ANIM_OFF);
		}
	}
	if (ui.format_dialog_open && ui.manage_detail) build_format_dialog();
	else ui.format_dialog_open = false;
	build_banner();
	ui_error_check();
}

/**
 * @brief Lightweight refresh pass used by the UI runtime task.
 *
 * @details Takes a snapshot and updates only the in-place parts of the current
 *          page: Direct page values, proxy controls, progress banner, detection
 *          spinner, and the error sheet. It never rebuilds the tree.
 *
 * @param[in] argument Unused task argument.
 */
void cardsight_ui_runtime(void *argument)
{
	(void)argument;
	cardsight_snapshot(&ui.model);
	ui_update_direct();
	ui_update_proxy_controls();
	ui_update_progress();
	ui_update_detecting();
	ui_error_check();
}
