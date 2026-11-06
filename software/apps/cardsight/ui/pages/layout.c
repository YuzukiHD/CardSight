// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

void layout_open_event(lv_event_t *event)
{
	(void)event;
	ui.layout_editor = true;
	ui_schedule_rebuild();
}

static void layout_table_event(lv_event_t *event)
{
	(void)event;
	ui.layout_table = ui.layout_table == BLK_PARTITION_TABLE_MBR ? BLK_PARTITION_TABLE_GPT : BLK_PARTITION_TABLE_MBR;
	if (ui.layout_table == BLK_PARTITION_TABLE_MBR && ui.layout_count > 4U) {
		ui.layout_count = 4U;
	}
	ui_schedule_rebuild();
}

static void layout_count_event(lv_event_t *event)
{
	intptr_t delta = (intptr_t)lv_event_get_user_data(event);
	unsigned int maximum;
	if (ui.layout_count == 0U) ui.layout_count = 1U;
	maximum = ui.layout_table == BLK_PARTITION_TABLE_MBR ? 4U : 8U;
	if (delta < 0 && ui.layout_count > 1U) {
		ui.layout_count--;
	} else if (delta > 0 && ui.layout_count < maximum) {
		if (ui.layout_mib[ui.layout_count - 1U] == 0U) {
			ui.layout_mib[ui.layout_count - 1U] = 512U;
		}
		ui.layout_count++;
	}
	ui.layout_mib[ui.layout_count - 1U] = 0U;
	ui_schedule_rebuild();
}

static void layout_size_event(lv_event_t *event)
{
	static const uint32_t sizes[] = { 512U, 1024U, 2048U, 4096U, 8192U, 0U };
	unsigned int entry = (unsigned int)(uintptr_t)lv_event_get_user_data(event);
	uint32_t next_size = sizes[0];
	/* Only the last partition can take the rest of the card; the others keep an explicit size. */
	unsigned int cycle = entry + 1U == ui.layout_count ? ARRAY_SIZE(sizes) : ARRAY_SIZE(sizes) - 1U;

	for (unsigned int index = 0U; index < cycle; index++) {
		if (ui.layout_mib[entry] == sizes[index]) {
			next_size = sizes[(index + 1U) % cycle];
			break;
		}
	}
	ui.layout_mib[entry] = next_size;
	ui_schedule_rebuild();
}

static void layout_fs_event(lv_event_t *event)
{
	unsigned int entry = (unsigned int)(uintptr_t)lv_event_get_user_data(event);
	ui.layout_content[entry] = (ui.layout_content[entry] + 1U) % 3U;
	ui_schedule_rebuild();
}

static void layout_apply_event(lv_event_t *event)
{
	struct cardsight_request request = { .type = CARDSIGHT_REQUEST_LAYOUT };

	(void)event;
	request.table = ui.layout_table;
	request.count = ui.layout_count;
	for (unsigned int index = 0U; index < request.count; index++) {
		uint32_t size_mib = ui.layout_mib[index] == 0U && index + 1U != request.count ? 512U : ui.layout_mib[index];

		request.entries[index].sectors = (uint64_t)size_mib * 2048U;
		request.entries[index].content = ui.layout_content[index];
		snprintf(request.entries[index].label, sizeof(request.entries[index].label), "CARD%u", index + 1U);
	}
	send_request(&request);
	ui.layout_editor = false;
	ui_schedule_rebuild();
}

static lv_obj_t *make_stepper_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data,
	int32_t horizontal_offset)
{
	lv_obj_t *button = make_button(parent, text, callback, data);

	lv_obj_set_size(button, APP_TOUCH_MIN, APP_TOUCH_MIN);
	lv_obj_set_style_radius(button, APP_TOUCH_MIN / 2, 0);
	lv_obj_align(button, LV_ALIGN_LEFT_MID, horizontal_offset, 0);
	return button;
}

/* Header holds the controls that shape the whole table: GPT/MBR and the partition count. */
static void build_layout_header(void)
{
	char count[20];
	int32_t width = ui_page_width();
	lv_obj_t *header = make_box(ui.content, lv_pct(100), APP_TOUCH_MIN);
	lv_obj_t *button;
	lv_obj_t *label;

	button = make_button(header, ui.layout_table == BLK_PARTITION_TABLE_GPT ? "GPT" : "MBR", layout_table_event, NULL);
	lv_obj_set_size(button, 68, APP_TOUCH_MIN);
	lv_obj_set_style_radius(button, APP_TOUCH_MIN / 2, 0);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_ACCENT), 0);
	lv_obj_align(button, LV_ALIGN_LEFT_MID, 0, 0);
	make_stepper_button(header, "-", layout_count_event, (void *)(intptr_t)-1, width - 2 * APP_TOUCH_MIN - 44);
	snprintf(count, sizeof(count), "%u区", ui.layout_count);
	label = make_label(header, count, UI_TEXT);
	lv_obj_set_style_text_font(label, &cardsight_font_20, 0);
	lv_obj_set_width(label, 44);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, width - APP_TOUCH_MIN - 44, 0);
	make_stepper_button(header, "+", layout_count_event, (void *)(intptr_t)1, width - APP_TOUCH_MIN);
}

static void build_layout_row(lv_obj_t *list, unsigned int index)
{
	char text[20];
	lv_obj_t *row = make_box(list, lv_pct(100), APP_ROW_HEIGHT);
	lv_obj_t *label;
	lv_obj_t *button;

	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 6, 0);
	snprintf(text, sizeof(text), "P%u", index + 1U);
	label = make_label(row, text, UI_MUTED);
	lv_obj_set_width(label, 28);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	button = make_button(row, content_name(ui.layout_content[index]), layout_fs_event, (void *)(uintptr_t)index);
	lv_obj_set_size(button, 80, APP_TOUCH_MIN);
	lv_obj_set_style_text_font(button, &cardsight_font_16, 0);
	lv_obj_set_style_text_color(button, lv_color_hex(fstype_color(content_name(ui.layout_content[index]))), 0);
	if (ui.layout_mib[index] == 0U) {
		strcpy(text, "剩余");
	} else {
		snprintf(text, sizeof(text), "%u MiB", ui.layout_mib[index]);
	}
	button = make_button(row, text, layout_size_event, (void *)(uintptr_t)index);
	lv_obj_set_height(button, APP_TOUCH_MIN);
	lv_obj_set_style_text_font(button, &cardsight_font_16, 0);
	lv_obj_set_flex_grow(button, 1);
}

void build_layout_editor(void)
{
	lv_obj_t *list;

	build_layout_header();
	list = make_list(APP_TOUCH_MIN + APP_GAP, 0);
	make_label(list, "写入会清除整张卡", UI_DANGER);
	for (unsigned int index = 0U; index < ui.layout_count; index++) {
		build_layout_row(list, index);
	}
}

/* Bottom bar of the editor: back, and a long press to write since it clears the whole card. */
void build_layout_bar(lv_obj_t *bar)
{
	int32_t back_width = 90;
	int32_t width = (int32_t)ui.width - 2 * APP_BAR_MARGIN;
	lv_obj_t *back = make_bar_button(bar, "返回", ui_back_event, back_width);

	lv_obj_set_style_radius(back, ui_bar_radius(), 0);
	lv_obj_set_pos(back, 0, 0);
	ui.layout_apply = make_bar_button(bar, "长按写入", NULL, width - back_width - 6);
	lv_obj_add_event_cb(ui.layout_apply, layout_apply_event, LV_EVENT_LONG_PRESSED, NULL);
	ui_style_button(ui.layout_apply, UI_BUTTON_DANGER);
	lv_obj_set_pos(ui.layout_apply, back_width + 6, 0);
	set_disabled(ui.layout_apply, ui.model.running || ui.model.busy || !ui.model.card_writable);
}
