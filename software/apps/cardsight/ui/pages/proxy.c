// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

static void style_selection(lv_obj_t *row, bool selected)
{
	lv_obj_t *mark = lv_obj_get_child(row, -1);

	lv_label_set_text(mark, selected ? "●" : "○");
	lv_obj_set_style_text_color(mark, lv_color_hex(selected ? UI_ACCENT : UI_MUTED), 0);
}

static void partition_event(lv_event_t *event)
{
	uint32_t partno = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
	lv_obj_t *checkbox = lv_event_get_target_obj(event);
	bool selected = lv_obj_has_state(checkbox, LV_STATE_CHECKED);

	if (selected) {
		ui.selected |= 1U << (partno - 1U);
	} else {
		ui.selected &= ~(1U << (partno - 1U));
	}
	style_selection(checkbox, selected);
	ui_update_proxy_controls();
}

static bool selected_read_only(void)
{
	for (size_t index = 0U; index < ui.model.partition_count; index++) {
		if ((ui.selected & (1U << (ui.model.partitions[index].partno - 1U))) != 0U && !ui.model.partitions[index].writable) {
			return true;
		}
	}
	return false;
}

void ui_update_proxy_controls(void)
{
	const char *text;
	bool disabled;
	lv_obj_t *label;

	if (ui.proxy_start == NULL) {
		return;
	}
	if (ui.model.running) {
		text = "安全停止";
	} else if (ui.selected == 0U) {
		text = "请选择分区";
	} else if (selected_read_only()) {
		text = "启动代理 只读";
	} else {
		text = "启动代理 读写";
	}
	disabled = ui.model.busy || (!ui.model.running && (!ui.model.card_present || ui.selected == 0U));
	label = lv_obj_get_child(ui.proxy_start, -1);
	lv_label_set_text(label, text);
	set_disabled(ui.proxy_start, disabled);
}

static void build_partition_row(lv_obj_t *list, const struct storage_proxy_partition_info *part)
{
	char title[48];
	char detail[48];
	char capacity[24];
	bool selected = (ui.selected & (1U << (part->partno - 1U))) != 0U;
	lv_obj_t *row = lv_button_create(list);
	lv_obj_t *label;

	capacity_text(part->sectors, capacity, sizeof(capacity));
	snprintf(title, sizeof(title), "P%u  %s", part->partno, part->status == 0 ? part->volume.fstype : "无法识别");
	snprintf(detail, sizeof(detail), "%s%s", capacity, part->writable ? "" : " 只读");
	lv_obj_set_size(row, lv_pct(100), APP_PART_ROW_HEIGHT);
	ui_style_button(row, UI_BUTTON_NORMAL);
	lv_obj_set_style_radius(row, 14, 0);
	disable_scroll(row);
	lv_obj_set_checkable(row, true);
	/* selected rows are a soft glass outline, not a filled card */
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_GLASS_TINT), LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(row, 40, LV_STATE_CHECKED);
	lv_obj_set_style_border_color(row, lv_color_hex(UI_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_border_opa(row, 120, LV_STATE_CHECKED);
	lv_obj_set_style_border_width(row, 1, LV_STATE_CHECKED);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_FULL, LV_STATE_CHECKED);
	label = make_label(row, title, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 44, 1);
	label = make_label(row, detail, part->writable ? UI_MUTED : UI_WARNING);
	lv_obj_set_style_text_font(label, &cardsight_font_16, 0);
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 44, 28);
	label = make_label(row, "○", UI_MUTED);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);
	if (selected) lv_obj_add_state(row, LV_STATE_CHECKED);
	style_selection(row, selected);
	set_disabled(row, ui.model.running || ui.model.busy);
	lv_obj_add_event_cb(row, partition_event, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)part->partno);
}

void build_proxy(void)
{
	lv_obj_t *list;

	list = make_list(0, APP_BUTTON_HEIGHT + APP_GAP);
	for (size_t index = 0U; index < ui.model.partition_count; index++) {
		build_partition_row(list, &ui.model.partitions[index]);
	}
	if (ui.model.partition_count == 0U) make_label(list, ui.model.card_present ? "无可用分区" : "请插入存储卡", UI_MUTED);

	ui.proxy_start = make_button(ui.content, "", start_event, NULL);
	lv_obj_set_width(ui.proxy_start, ui_wide_width());
	lv_obj_set_style_radius(ui.proxy_start, APP_BUTTON_HEIGHT / 2, 0);
	lv_obj_align(ui.proxy_start, LV_ALIGN_BOTTOM_MID, 0, 0);
	ui_glass_neutral(ui.proxy_start);
	ui_update_proxy_controls();
}
