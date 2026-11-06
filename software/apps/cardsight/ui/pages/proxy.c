// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

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

/* A partition row, built from a plain object with every style written explicitly: the theme
 * styles of buttons and their state cascade kept hiding the selection outline. */
/* The selection mark is drawn, not a glyph: the hollow circle character is not in the font
 * subset and used to show up as a box when a row was deselected. */
static void make_selection_dot(lv_obj_t *row)
{
	lv_obj_t *dot = lv_obj_create(row);

	lv_obj_set_size(dot, 14, 14);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(dot, 2, 0);
	lv_obj_set_style_border_color(dot, lv_color_hex(UI_MUTED), 0);
	lv_obj_set_style_pad_all(dot, 0, 0);
	lv_obj_set_style_outline_width(dot, 0, 0);
	lv_obj_set_style_shadow_width(dot, 0, 0);
	disable_scroll(dot);
	lv_obj_set_clickable(dot, false);
	lv_obj_align(dot, LV_ALIGN_LEFT_MID, 14, 0);
}

static void style_row(lv_obj_t *row, bool selected)
{
	lv_obj_t *dot = lv_obj_get_child(row, -1);

	lv_obj_set_style_bg_color(dot, lv_color_hex(UI_ACCENT), 0);
	lv_obj_set_style_bg_opa(dot, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_color(dot, lv_color_hex(selected ? UI_ACCENT : UI_MUTED), 0);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_GLASS_TINT), 0);
	lv_obj_set_style_bg_opa(row, selected ? 40 : LV_OPA_TRANSP, 0);
	/* no outline at all: the filled dot and the soft tint carry the selection */
	lv_obj_set_style_border_width(row, 0, 0);
}

static void partition_event(lv_event_t *event)
{
	uint32_t partno = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
	lv_obj_t *row = lv_event_get_current_target_obj(event);
	uint32_t mask = 1U << (partno - 1U);
	bool selected;

	if (ui.model.running || ui.model.busy) {
		return;
	}
	selected = (ui.selected & mask) == 0U;
	if (selected) {
		ui.selected |= mask;
	} else {
		ui.selected &= ~mask;
	}
	style_row(row, selected);
	ui_update_proxy_controls();
}

static void build_partition_row(lv_obj_t *list, const struct storage_proxy_partition_info *part)
{
	char title[48];
	char detail[48];
	char capacity[24];
	bool selected = (ui.selected & (1U << (part->partno - 1U))) != 0U;
	lv_obj_t *row = lv_obj_create(list);
	lv_obj_t *label;

	capacity_text(part->sectors, capacity, sizeof(capacity));
	snprintf(title, sizeof(title), "P%u  %s", part->partno, part->status == 0 ? part->volume.fstype : "无法识别");
	snprintf(detail, sizeof(detail), "%s%s", capacity, part->writable ? "" : " 只读");
	lv_obj_set_size(row, lv_pct(100), APP_PART_ROW_HEIGHT);
	lv_obj_set_style_radius(row, APP_CARD_RADIUS, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_outline_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_transform_width(row, 0, 0);
	lv_obj_set_style_transform_height(row, 0, 0);
	disable_scroll(row);
	lv_obj_set_clickable(row, true);
	label = make_label(row, title, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 44, 1);
	label = make_label(row, detail, part->writable ? UI_MUTED : UI_WARNING);
	lv_obj_set_style_text_font(label, &cardsight_font_16, 0);
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 44, 28);
	make_selection_dot(row);
	style_row(row, selected);
	lv_obj_add_event_cb(row, partition_event, LV_EVENT_CLICKED, (void *)(uintptr_t)part->partno);
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
	lv_obj_set_style_radius(ui.proxy_start, APP_CARD_RADIUS, 0);
	lv_obj_align(ui.proxy_start, LV_ALIGN_BOTTOM_MID, 0, 0);
	ui_glass_neutral(ui.proxy_start);
	ui_update_proxy_controls();
}
