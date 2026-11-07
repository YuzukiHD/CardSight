// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file manage.c
 * @brief Manage page: card capacity strip, partition list, and format dialog.
 *
 * The page shows a proportional strip of the card above a list of partitions,
 * with a quiet entry at the end that opens the whole-card layout editor. The
 * partition detail view offers quick and full format through a confirmation
 * dialog.
 */
#include "ui/ui_internal.h"

/** @brief Height of the proportional card strip, in pixels. */
#define MANAGE_BAR_HEIGHT 12

/**
 * @brief Open the detail view of the clicked partition.
 *
 * @param[in] event LVGL click event carrying the partition number.
 */
static void manage_partition_event(lv_event_t *event)
{
	ui.manage_partition = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
	ui.manage_detail = true;
	ui_schedule_rebuild();
}

/**
 * @brief Open the format confirmation for a chosen filesystem.
 *
 * @details Stores the content type, marks the dialog open, and builds it
 *          immediately because the click handler is already outside the LVGL
 *          input dispatch.
 *
 * @param[in] event LVGL click event carrying the content type as user data.
 */
static void format_choose_event(lv_event_t *event)
{
	ui.format_content = (enum blk_partition_content)(uintptr_t)lv_event_get_user_data(event);
	ui.format_dialog_open = true;
	build_format_dialog();
}

/**
 * @brief Close the format dialog without formatting.
 *
 * @details Ignored while a request is busy. Clears the open flag, deletes the
 *          dialog asynchronously, and restores the page content pointer.
 *
 * @param[in] event LVGL click event; not inspected.
 */
static void format_cancel_event(lv_event_t *event)
{
	(void)event;
	if (ui.model.busy) {
		return;
	}
	ui.format_dialog_open = false;
	if (ui.format_dialog != NULL) {
		lv_obj_delete_async(ui.format_dialog);
		ui.format_dialog = NULL;
	}
	ui.content = ui.tiles[(uint32_t)ui.mode];
}

/**
 * @brief Submit the format request chosen in the dialog.
 *
 * @details The user data distinguishes quick from full format: non-NULL means
 *          full. The request carries the partition and content type; the dialog
 *          and the detail view close and the page is rebuilt while the worker
 *          runs the format.
 *
 * @param[in] event LVGL click event carrying the full-format marker.
 */
static void format_start_event(lv_event_t *event)
{
	struct cardsight_request request = { .type = CARDSIGHT_REQUEST_FORMAT };

	request.partition = ui.manage_partition;
	request.content = ui.format_content;
	request.full = lv_event_get_user_data(event) != NULL;
	send_request(&request);
	ui.format_dialog_open = false;
	ui.manage_detail = false;
	ui_schedule_rebuild();
}

/* One full-width option; what it does is explained in the dialog text above. */
/**
 * @brief Create one full-width format option button.
 *
 * @details The primary option is filled in the background colour; the other is
 *          a danger-tinted option. What each one does is explained in the
 *          dialog text above. Both are disabled while a transfer runs, while
 *          the UI is busy, or when the card is read-only.
 *
 * @param[in,out] parent  Dialog receiving the option.
 * @param[in]     title   Option label.
 * @param[in]     full    True to request a full format.
 * @param[in]     primary True for the primary visual variant.
 * @param[in]     top     Y position in pixels.
 */
static void make_option(lv_obj_t *parent, const char *title, bool full, bool primary, int32_t top)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_t *label;
	uint32_t color = primary ? UI_BACKGROUND : UI_DANGER;

	lv_obj_set_size(button, ui_page_width(), APP_ROW_HEIGHT);
	lv_obj_set_pos(button, ui_margin(), top);
	ui_style_button(button, primary ? UI_BUTTON_PRIMARY : UI_BUTTON_DANGER);
	lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
	disable_scroll(button);
	label = make_label(button, title, color);
	lv_obj_center(label);
	lv_obj_add_event_cb(button, format_start_event, LV_EVENT_CLICKED, full ? (void *)1 : NULL);
	set_disabled(button, ui.model.running || ui.model.busy || !ui.model.card_writable);
}

/* Quick format recreates metadata; full format also overwrites all partition sectors. */
/**
 * @brief Build the format confirmation dialog.
 *
 * @details A full-screen background with the partition and filesystem in the
 *          title, a danger-coloured warning, the quick-format primary option,
 *          the full-format danger option, and a bottom back button. The busy
 *          banner is raised above the dialog so progress stays visible.
 */
void build_format_dialog(void)
{
	char text[64];
	lv_obj_t *dialog = lv_obj_create(ui.screen);
	lv_obj_t *label;
	lv_obj_t *back;
	int32_t top = ui_margin();

	ui.format_dialog = dialog;
	lv_obj_set_size(dialog, ui.width, ui.height);
	lv_obj_set_pos(dialog, 0, 0);
	ui_style_card(dialog, UI_BACKGROUND, 0);
	lv_obj_set_style_pad_all(dialog, 0, 0);
	disable_scroll(dialog);
	snprintf(text, sizeof(text), "格式化 P%u  %s", ui.manage_partition, content_name(ui.format_content));
	label = make_label(dialog, text, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_set_pos(label, ui_margin() + 4, top);
	label = make_label(dialog, "将清除分区数据。完全格式化会覆盖整个分区，耗时更长。", UI_DANGER);
	lv_obj_set_width(label, ui_page_width() - 8);
	lv_obj_set_pos(label, ui_margin() + 4, top + 34);
	make_option(dialog, "格式化", false, true, top + 112);
	make_option(dialog, "完全格式化", true, false, top + 112 + APP_ROW_HEIGHT + 6);
	back = make_bar_button(dialog, "返回", format_cancel_event, (int32_t)ui.width - 2 * APP_BAR_MARGIN);
	lv_obj_set_pos(back, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
	ui.content = dialog;
	if (ui.banner != NULL) {
		lv_obj_move_foreground(ui.banner);
	}
}

/*
 * Proportional strip of the card: one segment per partition in the filesystem colour. It is docked
 * above the list, so scrolling the partitions does not move the picture of the card.
 */
/**
 * @brief Draw the proportional strip of the card.
 *
 * @details One segment per partition, sized in proportion to its sector count
 *          and coloured by filesystem, with a two-pixel gap between segments
 *          and a minimum width of six pixels. Segments are clipped to the bar
 *          width. The strip is docked above the list, so scrolling the
 *          partitions does not move the picture of the card.
 */
static void build_capacity_bar(void)
{
	int32_t width = ui_page_width();
	int32_t segment_left = 0;
	lv_obj_t *bar = make_card(ui.content, width, MANAGE_BAR_HEIGHT, UI_SURFACE);

	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_radius(bar, MANAGE_BAR_HEIGHT / 2, 0);
	for (size_t index = 0U; index < ui.model.partition_count && ui.model.sectors != 0U; index++) {
		const struct storage_proxy_partition_info *part = &ui.model.partitions[index];
		int32_t segment = (int32_t)((uint64_t)part->sectors * (uint64_t)width / ui.model.sectors);
		lv_obj_t *fill;

		segment = MAX(segment, 6);
		if (segment_left + segment > width) segment = width - segment_left;
		if (segment <= 0) break;
		fill = make_card(bar, segment > 2 ? segment - 2 : segment, MANAGE_BAR_HEIGHT,
			part->status == 0 ? fstype_color(part->volume.fstype) : UI_MUTED);
		lv_obj_set_style_radius(fill, MANAGE_BAR_HEIGHT / 2, 0);
		lv_obj_set_pos(fill, segment_left, 0);
		segment_left += segment;
	}
}

/**
 * @brief Fill the partition list with one row per partition.
 *
 * @details Each row shows a colour dot, the partition number and filesystem,
 *          the size, and a read-only marker. Every row opens the partition
 *          detail view. An empty card shows a hint instead.
 *
 * @param[in,out] list Scrolling list receiving the rows.
 */
static void build_partition_rows(lv_obj_t *list)
{

	for (size_t index = 0; index < ui.model.partition_count; index++) {
		char text[64];
		char capacity[24];
		const struct storage_proxy_partition_info *part = &ui.model.partitions[index];
		lv_obj_t *button = lv_button_create(list);
		lv_obj_t *dot = make_card(button, 10, 10, part->status == 0 ? fstype_color(part->volume.fstype) : UI_MUTED);
		lv_obj_t *label;

		capacity_text(part->sectors, capacity, sizeof(capacity));
		lv_obj_set_size(button, lv_pct(100), APP_PART_ROW_HEIGHT);
		ui_style_button(button, UI_BUTTON_NORMAL);
		lv_obj_set_style_radius(button, 22, 0);
		disable_scroll(button);
		lv_obj_set_style_radius(dot, 5, 0);
		lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
		snprintf(text, sizeof(text), "P%u  %s", part->partno, part->status == 0 ? part->volume.fstype : "无法识别");
		label = make_label(button, text, UI_TEXT);
		lv_obj_align(label, LV_ALIGN_TOP_LEFT, 38, 1);
		snprintf(text, sizeof(text), "%s%s", capacity, part->writable ? "" : " 只读");
		label = make_label(button, text, part->writable ? UI_MUTED : UI_WARNING);
		lv_obj_set_style_text_font(label, &cardsight_font_16, 0);
		lv_obj_align(label, LV_ALIGN_TOP_LEFT, 38, 28);
		lv_obj_add_event_cb(button, manage_partition_event, LV_EVENT_CLICKED, (void *)(uintptr_t)part->partno);
	}
	if (ui.model.partition_count == 0U) make_label(list, ui.model.card_present ? "无可用分区" : "请插入存储卡", UI_MUTED);
}

/* The editor is a rare action, so it is a quiet row at the end of the list, not a docked button. */
/**
 * @brief Add the whole-card layout entry at the end of the list.
 *
 * @details The editor is a rare action, so it is a quiet glass row at the end
 *          of the list rather than a docked button. It is disabled while a
 *          transfer runs, while busy, or on a read-only card.
 *
 * @param[in,out] list Scrolling list receiving the row.
 */
static void build_layout_entry(lv_obj_t *list)
{
	lv_obj_t *button = make_button(list, "格式化整卡", layout_open_event, NULL);

	lv_obj_set_size(button, lv_pct(100), APP_ROW_HEIGHT);
	lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
	ui_glass_neutral(button);
	set_disabled(button, ui.model.running || ui.model.busy || !ui.model.card_writable);
}

/**
 * @brief Build the partition detail view with one format action per filesystem.
 *
 * @details Looks up whether the selected partition itself is writable, then
 *          adds a danger-styled row for each filesystem. The rows are disabled
 *          unless the card and the partition are both writable and the UI is
 *          idle.
 */
static void build_format_detail(void)
{
	/** @brief Filesystems offered in the partition detail view. */
	static const enum blk_partition_content contents[] = { BLK_PARTITION_FAT32, BLK_PARTITION_EXFAT, BLK_PARTITION_EXT4 };
	bool writable = false;
	char text[64];
	lv_obj_t *list;

	for (size_t index = 0; index < ui.model.partition_count; index++) {
		if (ui.model.partitions[index].partno == ui.manage_partition)
			writable = ui.model.partitions[index].writable;
	}
	list = make_list(0, 0);
	for (uint32_t index = 0U; index < ARRAY_SIZE(contents); index++) {
		lv_obj_t *button;

		snprintf(text, sizeof(text), "格式化 %s", content_name(contents[index]));
		button = make_button(list, text, format_choose_event, (void *)(uintptr_t)contents[index]);
		lv_obj_set_size(button, lv_pct(100), APP_ROW_HEIGHT);
		lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
		ui_style_button(button, UI_BUTTON_DANGER);
		set_disabled(button, ui.model.running || ui.model.busy || !ui.model.card_writable || !writable);
	}
}

/**
 * @brief Build the Manage page for the current mode.
 *
 * @details Dispatches to the layout editor when it is open, the format detail
 *          when a partition is selected, and otherwise the capacity strip plus
 *          the partition list and layout entry.
 */
void build_manage(void)
{
	lv_obj_t *list;

	if (ui.layout_editor) { build_layout_editor(); return; }
	if (ui.manage_detail) { build_format_detail(); return; }
	build_capacity_bar();
	list = make_list(MANAGE_BAR_HEIGHT + APP_GAP, 0);
	build_partition_rows(list);
	build_layout_entry(list);
}
