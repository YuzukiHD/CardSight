// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

struct cardsight_ui ui;
static void ui_clear_page_refs(void)
{
	ui.tabbar = NULL;
	ui.tab_indicator = NULL;
	ui.bar = NULL;
	ui.layout_apply = NULL;
	ui.calibration = NULL;
	memset(ui.tab_buttons, 0, sizeof(ui.tab_buttons));
	ui.banner = NULL;
	ui.banner_fill = NULL;
	ui.banner_label = NULL;
	ui.format_dialog = NULL;
	ui.direct_status = NULL;
	ui.direct_status_chip = NULL;
	ui.rate_read = NULL;
	ui.rate_write = NULL;
	ui.proxy_start = NULL;
}

static void ui_async_rebuild(void *argument)
{
	(void)argument;
	ui.rebuild_pending = false;
	cardsight_ui_refresh(NULL);
}

void ui_schedule_rebuild(void)
{
	if (!ui.rebuild_pending) {
		ui.rebuild_pending = lv_async_call(ui_async_rebuild, NULL) == LV_RESULT_OK;
	}
}

static void default_layout(void)
{
	uint64_t available_mib = ui.model.sectors / 2048U;
	unsigned int count = ui.model.partition_count == 0U ? 3U : (unsigned int)MIN(ui.model.partition_count, 3U);

	ui.layout_count = count;
	for (unsigned int index = 0U; index < APP_LAYOUT_MAX; index++) {
		ui.layout_mib[index] = index + 1U == count ? 0U : (uint32_t)((available_mib - 1U) / count);
		ui.layout_content[index] = index == 0U ? BLK_PARTITION_FAT32 :
			index == 1U ? BLK_PARTITION_EXFAT : BLK_PARTITION_EXT4;
	}
}

#define TAB_PAD 2

static int32_t bar_width(void)
{
	return (int32_t)ui.width - 2 * APP_BAR_MARGIN;
}

static int32_t tab_width(void)
{
	return (bar_width() - 2 - 2 * TAB_PAD) / (int32_t)APP_TAB_COUNT;
}

static enum cardsight_mode tab_shown;

static void tab_anim_x(void *object, int32_t value)
{
	lv_obj_set_x(object, value);
}

/* Slide the highlight to the active tab and recolour the labels. */
void ui_update_tabs(void)
{
	int32_t target;

	if (ui.tabbar == NULL) {
		return;
	}
	target = TAB_PAD + (int32_t)ui.mode * tab_width();
	for (unsigned int index = 0U; index < ARRAY_SIZE(ui.tab_buttons); index++) {
		lv_obj_set_style_text_color(ui.tab_buttons[index],
			lv_color_hex(ui.mode == (enum cardsight_mode)index ? UI_BACKGROUND : UI_TEXT), 0);
	}
	if (tab_shown == ui.mode) {
		lv_anim_delete(ui.tab_indicator, tab_anim_x);
		lv_obj_set_x(ui.tab_indicator, target);
		return;
	}
	tab_shown = ui.mode;
	lv_anim_t animation;

	lv_anim_init(&animation);
	lv_anim_set_var(&animation, ui.tab_indicator);
	lv_anim_set_exec_cb(&animation, tab_anim_x);
	lv_anim_set_values(&animation, lv_obj_get_x(ui.tab_indicator), target);
	lv_anim_set_duration(&animation, 160);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

static void tab_event(lv_event_t *event)
{
	/* A running request owns the screen; leaving its page would hide its progress. */
	if (ui.model.busy) {
		return;
	}
	show_page((enum cardsight_mode)(uintptr_t)lv_event_get_user_data(event));
}

/* Four large tabs along the bottom; the outer pill follows the glass corners. */
static void build_tab_bar(void)
{
	static const char *const names[APP_TAB_COUNT] = { "直连", "代理", "磁盘", "信息" };
	int32_t width = tab_width();
	/* Children are placed inside the 1 px border, so the highlight margin is measured from there. */
	int32_t inner_height = APP_BAR_HEIGHT - 2 - 2 * TAB_PAD;
	int32_t inner_radius = MAX(ui_bar_radius() - TAB_PAD, 8);

	ui.tabbar = make_card(ui.bar, bar_width(), APP_BAR_HEIGHT, UI_SURFACE);
	lv_obj_set_style_radius(ui.tabbar, ui_bar_radius(), 0);
	lv_obj_set_style_border_width(ui.tabbar, 1, 0);
	lv_obj_set_style_border_color(ui.tabbar, lv_color_hex(UI_OUTLINE), 0);
	ui.tab_indicator = make_card(ui.tabbar, width, inner_height, UI_ACCENT);
	lv_obj_set_style_radius(ui.tab_indicator, inner_radius, 0);
	lv_obj_set_pos(ui.tab_indicator, TAB_PAD + (int32_t)ui.mode * width, TAB_PAD);
	tab_shown = ui.mode;
	for (unsigned int index = 0U; index < ARRAY_SIZE(names); index++) {
		lv_obj_t *tab = make_button(ui.tabbar, names[index], tab_event, (void *)(uintptr_t)index);

		lv_obj_set_size(tab, width, inner_height);
		lv_obj_set_pos(tab, TAB_PAD + (int32_t)index * width, TAB_PAD);
		ui_style_button(tab, UI_BUTTON_GHOST);
		lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(tab, 0, 0);
		lv_obj_set_style_radius(tab, inner_radius, 0);
		ui.tab_buttons[index] = tab;
		set_disabled(tab, ui.model.busy);
	}
}

void ui_back_event(lv_event_t *event)
{
	(void)event;
	if (ui.model.busy) {
		return;
	}
	ui.layout_editor = false;
	ui.manage_detail = false;
	ui_schedule_rebuild();
}

/* Sub pages swap the tabs for a full width back button. */
static void build_bar(void)
{
	ui.bar = make_box(ui.screen, bar_width(), APP_BAR_HEIGHT);
	lv_obj_set_pos(ui.bar, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
	if (ui.layout_editor) {
		build_layout_bar(ui.bar);
	} else if (ui.manage_detail) {
		make_bar_button(ui.bar, "返回", ui_back_event, bar_width());
	} else {
		build_tab_bar();
	}
}

void show_page(enum cardsight_mode mode)
{
	for (unsigned int index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		lv_obj_set_hidden(ui.tiles[index], index != (unsigned int)mode);
	}
	ui.mode = mode;
	if (ui.layout_editor || ui.manage_detail) {
		ui.layout_editor = false;
		ui.manage_detail = false;
		ui_schedule_rebuild();
	}
	ui.content = ui.tiles[(unsigned int)mode];
	ui_update_tabs();
}

/* Rounded outline of the glass and the bar inset, to line up against the real corners. */
static void calibration_event(lv_event_t *event)
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
	snprintf(text, sizeof(text), "屏幕圆角 %ld px", (long)ui.screen_radius);
	label = make_label(ui.calibration, text, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -14);
	label = make_label(ui.calibration, "左侧 减小   中间 完成   右侧 增大", UI_MUTED);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 18);
}

static void banner_event(lv_event_t *event)
{
	(void)event;
	ui.error_acknowledged = true;
	if (ui.banner != NULL) {
		lv_obj_delete_async(ui.banner);
		ui.banner = NULL;
	}
}

/* Fill and text of the busy banner; called again for every progress step without a rebuild. */
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

/* Progress while a request runs, or the last failure until it is tapped away. */
static void build_banner(void)
{
	bool failed = !ui.model.busy;
	uint32_t color = failed ? UI_DANGER : UI_ACCENT;

	if (ui.model.busy) {
		ui.error_acknowledged = false;
	}
	if (!ui.model.busy && (ui.model.error == 0 || ui.error_acknowledged)) {
		return;
	}
	ui.banner = lv_obj_create(ui.screen);
	lv_obj_set_size(ui.banner, bar_width(), APP_BAR_HEIGHT);
	lv_obj_set_pos(ui.banner, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
	ui_style_card(ui.banner, UI_SURFACE, ui_bar_radius());
	lv_obj_set_style_bg_color(ui.banner, ui_tint(color, 60), 0);
	lv_obj_set_style_border_color(ui.banner, lv_color_hex(color), 0);
	lv_obj_set_style_border_width(ui.banner, 2, 0);
	lv_obj_set_style_pad_all(ui.banner, 0, 0);
	disable_scroll(ui.banner);
	lv_obj_set_clickable(ui.banner, failed);
	lv_obj_add_event_cb(ui.banner, banner_event, LV_EVENT_CLICKED, NULL);
	ui.banner_fill = make_card(ui.banner, 0, APP_BAR_HEIGHT - 4, color);
	lv_obj_set_style_bg_opa(ui.banner_fill, LV_OPA_50, 0);
	lv_obj_set_style_radius(ui.banner_fill, ui_bar_radius() - 2, 0);
	lv_obj_set_pos(ui.banner_fill, 0, 0);
	ui.banner_label = make_label(ui.banner, "", UI_TEXT);
	lv_obj_set_style_text_font(ui.banner_label, &cardsight_font_20, 0);
	if (failed) {
		lv_label_set_text_fmt(ui.banner_label, "%s (%d)", ui.model.operation, ui.model.error);
		lv_obj_set_hidden(ui.banner_fill, true);
	} else {
		ui_update_progress();
	}
	lv_obj_center(ui.banner_label);
}

static lv_obj_t *tile_list(unsigned int index)
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

void cardsight_ui_refresh(void *argument)
{
	int32_t scroll[APP_TAB_COUNT] = { 0 };

	struct sunxi_display_info display_info;

	(void)argument;
	cardsight_snapshot(&ui.model);
	if (!ui.selection_initialized && ui.model.partition_count != 0U) {
		for (size_t index = 0; index < ui.model.partition_count; index++)
			ui.selected |= 1U << (ui.model.partitions[index].partno - 1U);
		ui.selection_initialized = true;
	}
	if (ui.model.card_present && (ui.layout_count == 0U || ui.layout_sectors != ui.model.sectors)) {
		/* A different card: its size, partitions and selection are not the old card's. */
		default_layout();
		ui.layout_table = ui.model.table;
		if (ui.layout_sectors != 0U) {
			ui.selected = 0U;
			ui.selection_initialized = false;
			ui.layout_editor = false;
			ui.manage_detail = false;
		}
		ui.layout_sectors = ui.model.sectors;
	}
	if (sunxi_display_get_info(&display_info) == 0) {
		ui.width = display_info.width;
		ui.height = display_info.height;
		ui.landscape = ui.width >= ui.height;
	}
	if (ui.screen == NULL) {
		ui.screen = lv_screen_active();
		lv_obj_set_style_bg_color(ui.screen, lv_color_hex(UI_BACKGROUND), 0);
		lv_obj_set_style_text_font(ui.screen, &cardsight_font_16, 0);
		lv_obj_set_style_pad_all(ui.screen, 0, 0);
		disable_scroll(ui.screen);
	}
	/* A rebuild must not throw a list back to the top while the finger is moving it. */
	for (unsigned int index = 0U; index < APP_TAB_COUNT; index++) {
		lv_obj_t *list = tile_list(index);

		scroll[index] = list != NULL ? lv_obj_get_scroll_y(list) : 0;
	}
	lv_obj_clean(ui.screen);
	ui.shell = NULL;
	ui.page_host = NULL;
	memset(ui.tiles, 0, sizeof(ui.tiles));
	ui_clear_page_refs();
	if (ui.screen_radius == 0) {
		ui.screen_radius = APP_SCREEN_RADIUS;
	}
	ui.shell = lv_obj_create(ui.screen);
	lv_obj_set_size(ui.shell, ui.width, ui.height);
	lv_obj_set_pos(ui.shell, 0, 0);
	ui_style_card(ui.shell, UI_BACKGROUND, 0);
	lv_obj_set_style_pad_all(ui.shell, 0, 0);
	disable_scroll(ui.shell);

	ui.page_host = lv_obj_create(ui.shell);
	lv_obj_set_size(ui.page_host, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(ui.page_host, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(ui.page_host, 0, 0);
	lv_obj_set_style_pad_all(ui.page_host, 0, 0);
	disable_scroll(ui.page_host);
	for (unsigned int index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		ui.tiles[index] = lv_obj_create(ui.page_host);
		lv_obj_set_size(ui.tiles[index], lv_pct(100), lv_pct(100));
		lv_obj_set_pos(ui.tiles[index], 0, 0);
		lv_obj_set_style_bg_opa(ui.tiles[index], LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(ui.tiles[index], 0, 0);
		/* The tile spans the glass so a scrolling list can run to the edge; the padding keeps the
		 * content box where the pages lay out their fixed controls. */
		lv_obj_set_style_pad_all(ui.tiles[index], ui_margin(), 0);
		lv_obj_set_style_pad_bottom(ui.tiles[index], (int32_t)ui.height - ui_margin() - ui_page_height(), 0);
		disable_scroll(ui.tiles[index]);
	}
	ui.content = ui.tiles[CARDSIGHT_DIRECT];
	build_direct();
	ui.content = ui.tiles[CARDSIGHT_PROXY];
	build_proxy();
	ui.content = ui.tiles[CARDSIGHT_MANAGE];
	build_manage();
	ui.content = ui.tiles[CARDSIGHT_INFO];
	build_info();
	build_bar();
	ui.content = ui.tiles[(unsigned int)ui.mode];
	for (unsigned int index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		lv_obj_set_hidden(ui.tiles[index], index != (unsigned int)ui.mode);
	}
	ui_update_tabs();
	lv_obj_update_layout(ui.screen);
	for (unsigned int index = 0U; index < APP_TAB_COUNT; index++) {
		lv_obj_t *list = tile_list(index);

		if (list != NULL && scroll[index] != 0) {
			lv_obj_scroll_to_y(list, scroll[index], LV_ANIM_OFF);
		}
	}
	if (ui.format_dialog_open && ui.manage_detail) build_format_dialog();
	else ui.format_dialog_open = false;
	build_banner();
}

void cardsight_ui_runtime(void *argument)
{
	(void)argument;
	cardsight_snapshot(&ui.model);
	ui_update_direct();
	ui_update_proxy_controls();
	ui_update_progress();
}
