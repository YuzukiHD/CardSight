// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

#define MANAGE_BAR_HEIGHT 12

static void manage_partition_event(lv_event_t *event)
{
	ui.manage_partition = (unsigned int)(uintptr_t)lv_event_get_user_data(event);
	ui.manage_detail = true;
	ui_schedule_rebuild();
}

static void format_choose_event(lv_event_t *event)
{
	ui.format_content = (enum blk_partition_content)(uintptr_t)lv_event_get_user_data(event);
	ui.format_dialog_open = true;
	build_format_dialog();
}

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
	ui.content = ui.tiles[(unsigned int)ui.mode];
}

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
static void build_layout_entry(lv_obj_t *list)
{
	lv_obj_t *button = make_button(list, "编辑分区布局", layout_open_event, NULL);

	lv_obj_set_size(button, lv_pct(100), APP_ROW_HEIGHT);
	lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
	ui_style_button(button, UI_BUTTON_GHOST);
	lv_obj_set_style_border_color(button, lv_color_hex(UI_OUTLINE), 0);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_ACCENT), 0);
	lv_obj_set_style_text_font(button, &cardsight_font_16, 0);
	set_disabled(button, ui.model.running || ui.model.busy || !ui.model.card_writable);
}

static void build_format_detail(void)
{
	static const enum blk_partition_content contents[] = { BLK_PARTITION_FAT32, BLK_PARTITION_EXFAT, BLK_PARTITION_EXT4 };
	bool writable = false;
	char text[64];
	lv_obj_t *label;
	lv_obj_t *list;

	for (size_t index = 0; index < ui.model.partition_count; index++) {
		if (ui.model.partitions[index].partno == ui.manage_partition)
			writable = ui.model.partitions[index].writable;
	}
	list = make_list(0, 0);
	snprintf(text, sizeof(text), "P%u 格式化将清除数据", ui.manage_partition);
	label = make_label(list, text, UI_DANGER);
	lv_obj_set_style_pad_ver(label, 4, 0);
	for (unsigned int index = 0U; index < ARRAY_SIZE(contents); index++) {
		lv_obj_t *button;

		snprintf(text, sizeof(text), "格式化 %s", content_name(contents[index]));
		button = make_button(list, text, format_choose_event, (void *)(uintptr_t)contents[index]);
		lv_obj_set_size(button, lv_pct(100), APP_ROW_HEIGHT);
		lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
		ui_style_button(button, UI_BUTTON_DANGER);
		set_disabled(button, ui.model.running || ui.model.busy || !ui.model.card_writable || !writable);
	}
}

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
