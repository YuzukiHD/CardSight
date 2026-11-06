// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

static void rate_text(uint32_t bytes_per_second, char output[16])
{
	uint32_t kib = ui.model.running ? bytes_per_second / 1024U : 0U;

	snprintf(output, 16, "%u%s/s", kib >= 1024U ? kib / 1024U : kib, kib >= 1024U ? "M" : "K");
}

static const char *direct_state(uint32_t *color)
{
	if (!ui.model.card_present) {
		*color = UI_MUTED;
		return "未检测到卡";
	}
	if (!ui.model.running) {
		*color = UI_ACCENT;
		return "准备连接";
	}
	*color = ui.model.configured ? UI_SUCCESS : UI_WARNING;
	return ui.model.configured ? "已连接" : "等待主机";
}

/* Refresh only the live values, so a running transfer does not rebuild the page. */
void ui_update_direct(void)
{
	char rate[16];
	char text[32];
	uint32_t color;
	const char *state;

	if (ui.direct_status == NULL || ui.rate_read == NULL || ui.rate_write == NULL) {
		return;
	}
	state = direct_state(&color);
	snprintf(text, sizeof(text), "● %s", state);
	lv_label_set_text(ui.direct_status, text);
	lv_obj_set_style_text_color(ui.direct_status, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(ui.direct_status_chip, ui_tint(color, 40), 0);
	rate_text(ui.model.read_rate, rate);
	snprintf(text, sizeof(text), "读 %s", rate);
	lv_label_set_text(ui.rate_read, text);
	rate_text(ui.model.write_rate, rate);
	snprintf(text, sizeof(text), "写 %s", rate);
	lv_label_set_text(ui.rate_write, text);
}

/* Flex row that spans the hero card, so every line starts on the same inner edge. */
static lv_obj_t *make_hero_row(lv_obj_t *hero)
{
	lv_obj_t *row = make_box(hero, lv_pct(100), LV_SIZE_CONTENT);

	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	return row;
}

void build_direct(void)
{
	char capacity[24];
	char text[32];
	int32_t hero_height = ui_page_height() - APP_BUTTON_HEIGHT - APP_GAP;
	lv_obj_t *hero;
	lv_obj_t *row;
	lv_obj_t *number;
	lv_obj_t *unit;
	lv_obj_t *button;

	hero = make_glass_card(ui.content, -(ui_margin() - APP_EDGE_PAD), 0, ui_wide_width(), hero_height,
		APP_CARD_RADIUS);
	lv_obj_set_style_pad_hor(hero, 14, 0);
	lv_obj_set_style_pad_ver(hero, 12, 0);
	lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	row = make_hero_row(hero);
	{
		uint64_t capacity_bytes = ui.model.sectors * 512U;
		const char *unit_name;

		/* decimal units, as the card is sold: 31.9e9 bytes reads 32 GB */
		if (capacity_bytes >= 1000000000ULL) {
			snprintf(capacity, sizeof(capacity), "%llu",
				(unsigned long long)((capacity_bytes + 500000000ULL) / 1000000000ULL));
			unit_name = "GB";
		} else {
			snprintf(capacity, sizeof(capacity), "%llu",
				(unsigned long long)((capacity_bytes + 500000ULL) / 1000000ULL));
			unit_name = "MB";
		}
		number = make_label(row, ui.model.card_present ? capacity : "--", UI_TEXT);
		lv_obj_set_style_text_font(number, &cardsight_digits_32, 0);
		unit = make_label(row, unit_name, UI_MUTED);
	}
	lv_obj_set_style_pad_left(unit, 6, 0);
	lv_obj_set_style_pad_bottom(unit, 2, 0);

	ui.direct_status_chip = make_chip(hero, "", UI_MUTED);
	ui.direct_status = lv_obj_get_child(ui.direct_status_chip, 0);

	row = make_box(hero, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	ui.rate_read = make_label(row, "", UI_TEXT);
	lv_obj_set_width(ui.rate_read, lv_pct(48));
	ui.rate_write = make_label(row, "", UI_TEXT);
	lv_obj_set_width(ui.rate_write, lv_pct(48));

	row = make_box(hero, lv_pct(100), 24);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 6, 0);
	if (ui.model.card_present) {
		make_chip(row, ui.model.table == BLK_PARTITION_TABLE_GPT ? "GPT" : "MBR", UI_MUTED);
		snprintf(text, sizeof(text), "%u 区", (uint32_t)ui.model.partition_count);
		make_chip(row, text, UI_MUTED);
		make_chip(row, ui.model.card_writable ? "读写" : "只读",
			ui.model.card_writable ? UI_SUCCESS : UI_WARNING);
	} else {
		make_chip(row, "请插入存储卡", UI_MUTED);
	}
	ui_update_direct();

	button = make_button(ui.content, ui.model.running ? "安全停止" : "连接 USB", start_event, NULL);
	lv_obj_set_width(button, ui_wide_width());
	lv_obj_set_style_radius(button, APP_CARD_RADIUS, 0);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, 0);
	ui_glass_neutral(button);
	set_disabled(button, ui.model.busy || (!ui.model.running && !ui.model.card_present));
}
