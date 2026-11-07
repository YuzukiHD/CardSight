// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui.c
 * @brief Screen shell, tab bar, dialogs, and the top-level refresh and rebuild path.
 *
 * cardsight_ui_refresh() tears the LVGL screen down and rebuilds the tabs, all
 * page tiles, and the bottom bar from a snapshot of the application model.
 * Rebuilds are deferred through an LVGL async call so they never run inside an
 * input event, and the scroll position of every list is preserved across them.
 */
#include "ui/ui_internal.h"

/** @brief The single live UI instance; only the LVGL task mutates it. */
struct cardsight_ui ui;
/**
 * @brief Drop every cached widget pointer before a rebuild.
 *
 * @details The screen is about to be cleaned, so all pointers into the old tree
 *          become stale. The tab arrays are zeroed as well; pages refill their
 *          own pointers when they are rebuilt.
 */
static void ui_clear_page_refs(void)
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
static void ui_async_rebuild(void *argument)
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
 * @brief Seed the layout editor with a default three-partition split.
 *
 * @details Uses up to three partitions, gives each all but the last MiB of the
 *          card divided by the partition count, and assigns FAT32, exFAT, and
 *          ext4 in order. The last partition keeps a zero size, meaning "rest of
 *          the card".
 */
static void default_layout(void)
{
	uint64_t available_mib = ui.model.sectors / 2048U;
	uint32_t count = ui.model.partition_count == 0U ? 3U : (uint32_t)MIN(ui.model.partition_count, 3U);

	ui.layout_count = count;
	for (uint32_t index = 0U; index < APP_LAYOUT_MAX; index++) {
		ui.layout_mib[index] = index + 1U == count ? 0U : (uint32_t)((available_mib - 1U) / count);
		ui.layout_content[index] = index == 0U ? BLK_PARTITION_FAT32 :
			index == 1U ? BLK_PARTITION_EXFAT : BLK_PARTITION_EXT4;
	}
}

/** @brief Inset of the tab highlight and buttons from the bar edge in pixels. */
#define TAB_PAD 2

/**
 * @brief Return the width of the bottom bar inside the screen margins.
 *
 * @return Width in pixels.
 */
static int32_t bar_width(void)
{
	return (int32_t)ui.width - 2 * APP_BAR_MARGIN;
}

/**
 * @brief Return the width of one tab cell.
 *
 * @details Splits the bar's inner width, after the two-pixel border and the
 *          pads, evenly across the tabs.
 *
 * @return Width in pixels.
 */
static int32_t tab_width(void)
{
	return (bar_width() - 2 - 2 * TAB_PAD) / (int32_t)APP_TAB_COUNT;
}

/* The five tabs share the inner width; the last one is pinned to the right pad so the
 * outer gaps match instead of leaving the division remainder on the right. */
/**
 * @brief Return the X position of a tab inside the bar.
 *
 * @details Distributes the free space evenly between the first and last tab and
 *          pins the last tab to the right pad, so the outer gaps match instead
 *          of leaving the division remainder on the right.
 *
 * @param[in] index Tab index.
 * @return X position in pixels.
 */
static int32_t tab_x(uint32_t index)
{
	int32_t inner = bar_width() - 2 - 2 * TAB_PAD;
	int32_t width = tab_width();
	int32_t step = (inner - width) / (int32_t)(APP_TAB_COUNT - 1U);

	if (index + 1U >= APP_TAB_COUNT) {
		return TAB_PAD + inner - width;
	}
	return TAB_PAD + (int32_t)index * step;
}

/** @brief Page whose tab is currently highlighted; drives the slide animation. */
static enum cardsight_mode tab_shown;

/**
 * @brief Animation callback that slides the tab highlight.
 *
 * @details Moves the indicator and then realigns its glass backdrop so the
 *          blurred wallpaper stays registered while the highlight moves.
 *
 * @param[in,out] object Indicator object.
 * @param[in]     value  New X position in pixels.
 */
static void tab_anim_x(void *object, int32_t value)
{
	lv_obj_set_x(object, value);
	glass_highlight_move(ui.tab_indicator, ui.tab_indicator_backdrop);
}

/* The selected icon grows a little; the move is eased so it reads as one motion with the
 * sliding highlight. */
/**
 * @brief Animation callback that scales a tab icon.
 *
 * @param[in,out] object Icon being scaled.
 * @param[in]     value  New LVGL image scale (256 is 1:1).
 */
static void tab_icon_anim_scale(void *object, int32_t value)
{
	lv_image_set_scale(object, (uint32_t)value);
}

/**
 * @brief Animate a tab icon to a target scale.
 *
 * @details Returns early when the icon is already at the target. Without
 *          animations the scale is set directly; otherwise it eases over
 *          160 ms so the growth reads as one motion with the sliding highlight.
 *
 * @param[in,out] icon   Icon to scale; NULL is ignored.
 * @param[in]     target Target LVGL image scale (256 is 1:1).
 */
static void tab_icon_scale_to(lv_obj_t *icon, int32_t target)
{
	lv_anim_t animation;

	if (icon == NULL || lv_image_get_scale(icon) == target) {
		return;
	}
	if (!ui.animations) {
		lv_image_set_scale(icon, (uint32_t)target);
		return;
	}
	lv_anim_init(&animation);
	lv_anim_set_var(&animation, icon);
	lv_anim_set_exec_cb(&animation, tab_icon_anim_scale);
	lv_anim_set_values(&animation, lv_image_get_scale(icon), target);
	lv_anim_set_duration(&animation, 160);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

/* Slide the highlight to the active tab and recolour the labels. */
/**
 * @brief Slide the highlight to the active tab and recolour the labels.
 *
 * @details Every tab is coloured by selection and the selected icon is scaled
 *          up slightly. The highlight moves with an eased animation, unless the
 *          page did not change or animations are disabled, in which case it
 *          jumps to the target.
 */
void ui_update_tabs(void)
{
	int32_t target;

	if (ui.tabbar == NULL) {
		return;
	}
	target = tab_x((uint32_t)ui.mode);
	for (uint32_t index = 0U; index < ARRAY_SIZE(ui.tab_buttons); index++) {
		bool selected = ui.mode == (enum cardsight_mode)index;
		uint32_t color = selected ? UI_TEXT : UI_MUTED;

		lv_obj_set_style_text_color(ui.tab_buttons[index], lv_color_hex(color), 0);
		if (ui.tab_icons[index] != NULL) {
			lv_obj_set_style_image_recolor(ui.tab_icons[index], lv_color_hex(color), 0);
			lv_obj_set_style_image_recolor_opa(ui.tab_icons[index], LV_OPA_COVER, 0);
			tab_icon_scale_to(ui.tab_icons[index], selected ? 288 : 256);
		}
	}
	if (tab_shown == ui.mode || !ui.animations) {
		lv_anim_delete(ui.tab_indicator, tab_anim_x);
		lv_obj_set_x(ui.tab_indicator, target);
		glass_highlight_move(ui.tab_indicator, ui.tab_indicator_backdrop);
		tab_shown = ui.mode;
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

/**
 * @brief Click handler for a bottom tab.
 *
 * @details Does nothing while a request is busy, because leaving its page would
 *          hide its progress. Otherwise the tab's index, carried as user data,
 *          selects the page.
 *
 * @param[in] event LVGL click event carrying the tab index.
 */
static void tab_event(lv_event_t *event)
{
	/* A running request owns the screen; leaving its page would hide its progress. */
	if (ui.model.busy) {
		return;
	}
	show_page((enum cardsight_mode)(uintptr_t)lv_event_get_user_data(event));
}

/* Five icon tabs along the bottom; the outer pill follows the glass corners. */
/**
 * @brief Build the five-icon tab bar along the bottom.
 *
 * @details Wraps the bar in a glass card, then creates the sliding highlight
 *          with its own frosted backdrop, followed by one transparent ghost
 *          button per tab holding a scaled, recoloured icon. The buttons are
 *          disabled while a request is busy.
 */
static void build_tab_bar(void)
{
	/** @brief Icon of each tab, indexed like the pages. */
	static const lv_image_dsc_t *const icons[APP_TAB_COUNT] = {
		&cardsight_icon_usb, &cardsight_icon_proxy, &cardsight_icon_drive,
		&cardsight_icon_folder, &cardsight_icon_info,
	};
	int32_t width = tab_width();
	/* Children are placed inside the 1 px border, so the highlight margin is measured from there. */
	int32_t inner_height = APP_BAR_HEIGHT - 2 - 2 * TAB_PAD;
	int32_t inner_radius = MAX(ui_bar_radius() - TAB_PAD, 8);

	ui.tabbar = make_glass_card(ui.bar, 0, 0, bar_width(), APP_BAR_HEIGHT, ui_bar_radius());
	ui.tab_indicator = lv_obj_create(ui.tabbar);
	lv_obj_set_size(ui.tab_indicator, width, inner_height);
	lv_obj_set_pos(ui.tab_indicator, tab_x((uint32_t)ui.mode), TAB_PAD);
	ui.tab_indicator_backdrop = glass_highlight_attach(ui.tab_indicator, inner_radius);
	tab_shown = ui.mode;
	for (uint32_t index = 0U; index < ARRAY_SIZE(icons); index++) {
		lv_obj_t *tab = lv_button_create(ui.tabbar);
		lv_obj_t *icon;

		lv_obj_set_size(tab, width, inner_height);
		lv_obj_set_pos(tab, tab_x(index), TAB_PAD);
		ui_style_button(tab, UI_BUTTON_GHOST);
		lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(tab, 0, 0);
		lv_obj_set_style_radius(tab, inner_radius, 0);
		lv_obj_add_event_cb(tab, tab_event, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
		icon = lv_image_create(tab);
		lv_image_set_src(icon, icons[index]);
		lv_obj_center(icon);
		lv_obj_set_style_image_recolor(icon, lv_color_hex(UI_MUTED), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		lv_image_set_scale(icon, 256U);
		ui.tab_icons[index] = icon;
		ui.tab_buttons[index] = tab;
		disable_scroll(tab);
		set_disabled(tab, ui.model.busy);
	}
}

/**
 * @brief Common handler for the back gesture or button.
 *
 * @details Ignored while busy. On the Files page in detail view it navigates up
 *          one directory; elsewhere it leaves the layout editor or partition
 *          detail view and schedules a rebuild.
 *
 * @param[in] event LVGL event that triggered the back action.
 */
void ui_back_event(lv_event_t *event)
{
	(void)event;
	if (ui.model.busy) {
		return;
	}
	if (ui.mode == CARDSIGHT_FILES && ui.files_detail) {
		files_go_back();
		return;
	}
	ui.layout_editor = false;
	ui.manage_detail = false;
	ui_schedule_rebuild();
}

/* Sub pages swap the tabs for a full width back button. */
/**
 * @brief Build the bottom bar: layout actions, a back button, or the tabs.
 *
 * @details The bar is a transparent box that lets the glass card's shadow spill
 *          outside it. It hosts the layout editor actions, a full-width back
 *          button on detail pages, and the tab bar otherwise.
 */
static void build_bar(void)
{
	ui.bar = make_box(ui.screen, bar_width(), APP_BAR_HEIGHT);
	lv_obj_set_pos(ui.bar, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
	/* the glass card's shadow lives outside the bar; the screen still clips the outside */
	lv_obj_set_overflow_visible(ui.bar, true);
	if (ui.layout_editor) {
		build_layout_bar(ui.bar);
	} else if (ui.manage_detail || (ui.mode == CARDSIGHT_FILES && ui.files_detail)) {
		make_bar_button(ui.bar, "返回", ui_back_event, bar_width());
	} else {
		build_tab_bar();
	}
}

/**
 * @brief Switch the visible page and update the tab bar.
 *
 * @details Hides every other tile, leaves any open detail view, points the
 *          content host at the selected tile, and refreshes the tabs, header,
 *          and backdrop.
 *
 * @param[in] mode Page to show.
 */
void show_page(enum cardsight_mode mode)
{
	for (uint32_t index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		lv_obj_set_hidden(ui.tiles[index], index != (uint32_t)mode);
	}
	ui.mode = mode;
	if (ui.layout_editor || ui.manage_detail) {
		ui.layout_editor = false;
		ui.manage_detail = false;
		ui_schedule_rebuild();
	}
	ui.content = ui.tiles[(uint32_t)mode];
	ui_update_tabs();
	ui_update_header();
	ui_update_backdrop();
}

/* The identification happens before the block device exists, so the pages cannot tell. */
/**
 * @brief Show or hide the card-identification spinner.
 *
 * @details Identification happens before the block device exists, so the pages
 *          cannot tell; this overlay is driven straight from the model flag.
 */
void ui_update_detecting(void)
{
	if (ui.detecting_card != NULL) {
		lv_obj_set_hidden(ui.detecting_card, !ui.model.card_detecting);
	}
}

/**
 * @brief Select the wallpaper variant for the current page.
 *
 * @details Text-heavy pages get the frosted wallpaper, while the home page keeps
 *          the sharp artwork. The list fade strips are repainted afterwards.
 */
void ui_update_backdrop(void)
{
	/* Text-heavy pages get the frosted wallpaper; the home page stays sharp. */
	if (ui.background != NULL) {
		lv_image_set_src(ui.background, ui.mode == CARDSIGHT_DIRECT ? backdrop_image() :
			backdrop_text_image());
	}
	ui_refresh_list_fades();
}

/**
 * @brief Refresh the app bar title, item count, and card status.
 *
 * @details The title is the layout editor's name or the page name; the status
 *          shows "identifying", the formatted capacity, or "no card"; the count
 *          appears only on the Files detail view.
 */
void ui_update_header(void)
{
	/** @brief Page title of each tab, indexed by cardsight_mode. */
	static const char *const names[APP_TAB_COUNT] = { "直连", "代理", "磁盘", "文件", "信息" };
	char title[48];
	char count[24] = "";
	char status[24];

	if (ui.header_title == NULL) {
		return;
	}
	if (ui.layout_editor) {
		snprintf(title, sizeof(title), "格式化");
	} else {
		snprintf(title, sizeof(title), "%s", names[ui.mode]);
	}
	if (ui.model.card_detecting) {
		snprintf(status, sizeof(status), "识别中");
	} else if (ui.model.card_present) {
		capacity_text(ui.model.sectors, status, sizeof(status));
	} else {
		snprintf(status, sizeof(status), "无卡");
	}
	if (ui.mode == CARDSIGHT_FILES && ui.files_detail && ui.model.files.valid) {
		snprintf(count, sizeof(count), "%u 项", (uint32_t)ui.model.files.total);
	}
	lv_label_set_text(ui.header_title, title);
	lv_label_set_text(ui.header_count, count);
	lv_label_set_text(ui.header_status, status);
}

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
	snprintf(text, sizeof(text), "屏幕圆角 %ld px", (long)ui.screen_radius);
	label = make_label(ui.calibration, text, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -14);
	label = make_label(ui.calibration, "左侧 减小   中间 完成   右侧 增大", UI_MUTED);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 18);
}

/**
 * @brief Click handler that closes the settings sheet.
 *
 * @param[in] event LVGL click event; not inspected.
 */
static void settings_close_event(lv_event_t *event)
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
static void settings_open_async(void *argument)
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
	label = make_label(dialog, "设置", UI_TEXT);
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
	button = make_bar_button(dialog, "返回", settings_close_event, (int32_t)ui.width - 2 * APP_BAR_MARGIN);
	lv_obj_set_pos(button, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
}

/**
 * @brief Click handler that opens the settings sheet asynchronously.
 *
 * @param[in] event LVGL click event; not inspected.
 */
static void settings_event(lv_event_t *event)
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
static const char *error_title(enum cardsight_request_type request)
{
	switch (request) {
	case CARDSIGHT_REQUEST_SCAN:
	case CARDSIGHT_REQUEST_LIST:
		return "读取失败";
	case CARDSIGHT_REQUEST_FORMAT:
	case CARDSIGHT_REQUEST_LAYOUT:
		return "格式化失败";
	case CARDSIGHT_REQUEST_START_DIRECT:
	case CARDSIGHT_REQUEST_START_PROXY:
		return "连接失败";
	case CARDSIGHT_REQUEST_STOP:
		return "停止失败";
	default:
		return "操作失败";
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
static void error_close_event(lv_event_t *event)
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
static void error_open_async(void *argument)
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
	label = make_label(card, ui.model.operation[0] != '\0' ? ui.model.operation : "操作失败",
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
		snprintf(text, sizeof(text), "错误码: %d", ui.model.error);
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
		label = make_label(card, "请检查后重试", UI_MUTED);
		lv_obj_set_pos(label, 14, y);
	}
	button = make_bar_button(dialog, "返回", error_close_event,
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
static void ui_error_check(void)
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
static void build_banner(void)
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

/**
 * @brief Find the scrollable list inside a page tile.
 *
 * @details Returns the first direct child of the tile that LVGL reports as
 *          scrollable, which is the list built by make_list().
 *
 * @param[in] index Tile index.
 * @return The list object, or NULL when the tile has none.
 */
static lv_obj_t *tile_list(uint32_t index)
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
