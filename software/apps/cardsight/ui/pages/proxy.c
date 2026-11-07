// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file proxy.c
 * @brief Proxy page: choose partitions to export and start or stop the proxy.
 *
 * The list rows are plain objects styled explicitly rather than themed buttons,
 * because the button state cascade kept hiding the selection outline.
 */
#include "ui/ui_internal.h"

/**
 * @brief Check whether any selected partition is read-only.
 *
 * @details Walks the partitions and tests each one's bit in the selection mask.
 *
 * @return True when at least one selected partition cannot be written.
 */
static bool selected_read_only(void)
{
	for (size_t index = 0U; index < ui.model.partition_count; index++) {
		if ((ui.selected & (1U << (ui.model.partitions[index].partno - 1U))) != 0U && !ui.model.partitions[index].writable) {
			return true;
		}
	}
	return false;
}

/**
 * @brief Refresh the proxy start button text and enabled state.
 *
 * @details The label is "safe stop" while running, a choose-a-partition hint
 *          with no selection, and a read-write or read-only start label
 *          otherwise. The button is disabled while busy or, when idle, with no
 *          card or no selected partition.
 */
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
/**
 * @brief Add the drawn selection mark to a partition row.
 *
 * @details The mark is drawn, not a glyph: the hollow circle character is not
 *          in the font subset and used to show up as a box when a row was
 *          deselected. It is a 14-pixel outlined circle aligned to the left
 *          middle of the row.
 *
 * @param[in,out] row Partition row receiving the mark.
 */
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

/**
 * @brief Apply the selected or unselected look to a partition row.
 *
 * @details The dot fills with the accent colour when selected. The row itself
 *          gets an accent tint at 40 opacity when selected and no outline at
 *          all: the filled dot and the soft tint carry the selection.
 *
 * @param[in,out] row      Partition row to style.
 * @param[in]     selected True for the selected look.
 */
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

/**
 * @brief Toggle a partition's selection.
 *
 * @details Ignored while a transfer runs or the UI is busy. The partition's bit
 *          in the selection mask is flipped, the row restyled, and the proxy
 *          button updated.
 *
 * @param[in] event LVGL click event carrying the partition number.
 */
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

/**
 * @brief Build one selectable partition row.
 *
 * @details A transparent rounded object with the title, size and read-only
 *          marker, and the drawn selection dot. All button-like state visuals
 *          are written explicitly so the theme cannot override the selected
 *          tint. The row is wired to the selection toggle.
 *
 * @param[in,out] list List receiving the row.
 * @param[in]     part Partition snapshot to display.
 */
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

/**
 * @brief Build the Proxy page widget tree.
 *
 * @details Creates the scrolling partition list with room for the docked start
 *          button, adds one selectable row per partition, shows a hint when the
 *          card has none, and builds the glass start/stop button at the bottom.
 */
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
