// SPDX-License-Identifier: GPL-2.0-or-later
/* Whole-card format: one partition, a chosen table and filesystem. */

#include "ui/ui_internal.h"

void layout_open_event(lv_event_t *event)
{
	(void)event;
	ui.layout_editor = true;
	ui_schedule_rebuild();
}

static void layout_table_event(lv_event_t *event)
{
	uint32_t table = (uint32_t)(uintptr_t)lv_event_get_user_data(event);

	ui.layout_table = table != 0U ? BLK_PARTITION_TABLE_GPT : BLK_PARTITION_TABLE_MBR;
	ui_schedule_rebuild();
}

static void layout_fs_event(lv_event_t *event)
{
	ui.layout_content[0] = (enum blk_partition_content)(uintptr_t)lv_event_get_user_data(event);
	ui_schedule_rebuild();
}

static void layout_apply_event(lv_event_t *event)
{
	struct cardsight_request request = { .type = CARDSIGHT_REQUEST_LAYOUT, .count = 1U };

	(void)event;
	request.table = ui.layout_table;
	request.entries[0].sectors = 0U; /* the rest of the card */
	request.entries[0].content = ui.layout_content[0];
	snprintf(request.entries[0].label, sizeof(request.entries[0].label), "CARD1");
	send_request(&request);
	ui.layout_editor = false;
	ui_schedule_rebuild();
}

static lv_obj_t *make_segment(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, const char *text,
	bool selected, lv_event_cb_t callback, void *data)
{
	lv_obj_t *button = make_button(parent, text, callback, data);

	lv_obj_set_size(button, width, 32);
	lv_obj_set_style_radius(button, 16, 0);
	lv_obj_set_pos(button, x, y);
	if (selected) {
		static const lv_state_t states[] = {
			0, LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY,
		};

		ui_style_button(button, UI_BUTTON_PRIMARY);
		/* a solid accent pill with a bright ring makes the choice unmistakable */
		for (uint32_t index = 0U; index < ARRAY_SIZE(states); index++) {
			lv_obj_set_style_border_width(button, 2, states[index]);
			lv_obj_set_style_border_side(button, LV_BORDER_SIDE_FULL, states[index]);
			lv_obj_set_style_border_color(button, lv_color_hex(UI_TEXT), states[index]);
			lv_obj_set_style_border_opa(button, 230, states[index]);
			lv_obj_set_style_shadow_width(button, 8, states[index]);
			lv_obj_set_style_shadow_color(button, lv_color_hex(UI_ACCENT), states[index]);
			lv_obj_set_style_shadow_opa(button, 90, states[index]);
		}
	} else {
		ui_glass_neutral(button);
	}
	return button;
}

void build_layout_editor(void)
{
	static const char *const tables[] = { "MBR", "GPT" };
	static const enum blk_partition_content contents[] = {
		BLK_PARTITION_FAT32, BLK_PARTITION_EXFAT, BLK_PARTITION_EXT4,
	};
	char capacity[24];
	int32_t width = ui_page_width();
	lv_obj_t *card = lv_obj_create(ui.content);
	lv_obj_t *label;

	lv_obj_set_size(card, width, ui_page_height());
	lv_obj_set_pos(card, 0, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_clip_corner(card, true, 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	disable_scroll(card);
	glass_attach_tinted(card, 18, 220);

	label = make_label(card, "分区表", UI_MUTED);
	lv_obj_set_pos(label, 14, 8);
	for (uint32_t index = 0U; index < ARRAY_SIZE(tables); index++) {
		make_segment(card, 14 + (int32_t)index * 92, 26, 86, tables[index],
			(ui.layout_table == BLK_PARTITION_TABLE_GPT) == (index != 0U), layout_table_event,
			(void *)(uintptr_t)index);
	}

	label = make_label(card, "文件系统", UI_MUTED);
	lv_obj_set_pos(label, 14, 64);
	for (uint32_t index = 0U; index < ARRAY_SIZE(contents); index++) {
		make_segment(card, 14 + (int32_t)index * 62, 82, 56, content_name(contents[index]),
			ui.layout_content[0] == contents[index], layout_fs_event,
			(void *)(uintptr_t)contents[index]);
	}

	capacity_text(ui.model.sectors, capacity, sizeof(capacity));
	label = make_label(card, "容量", UI_MUTED);
	lv_obj_set_pos(label, 14, 122);
	label = make_label(card, capacity, UI_TEXT);
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -14, 122);

	label = make_label(card, "将清除整张卡数据", UI_DANGER);
	lv_obj_set_pos(label, 14, 146);
}

void build_layout_bar(lv_obj_t *bar)
{
	int32_t back_width = 90;
	int32_t width = (int32_t)ui.width - 2 * APP_BAR_MARGIN;
	lv_obj_t *back = make_bar_button(bar, "返回", ui_back_event, back_width);

	lv_obj_set_pos(back, 0, 0);
	ui.layout_apply = make_button(bar, "长按格式化", NULL, NULL);
	lv_obj_set_size(ui.layout_apply, width - back_width - 6, APP_BAR_HEIGHT);
	lv_obj_set_style_radius(ui.layout_apply, ui_bar_radius(), 0);
	lv_obj_set_pos(ui.layout_apply, back_width + 6, 0);
	ui_glass_action(ui.layout_apply, UI_DANGER);
	lv_obj_set_style_text_color(lv_obj_get_child(ui.layout_apply, -1), lv_color_hex(UI_DANGER), 0);
	lv_obj_add_event_cb(ui.layout_apply, layout_apply_event, LV_EVENT_LONG_PRESSED, NULL);
	set_disabled(ui.layout_apply, ui.model.running || ui.model.busy || !ui.model.card_writable);
}
