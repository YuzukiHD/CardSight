// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file direct.c
 * @brief Direct page: the card hero card, live transfer rates, and the USB action.
 *
 * The page renders one glass hero card with the capacity, the link state chip,
 * the read/write rates, and the card capability chips, plus the docked connect
 * or safe-stop button. A running transfer refreshes the values in place.
 */
#include "ui/ui_private.h"

/**
 * @brief Format a byte rate as "K/s" or "M/s" into a fixed buffer.
 *
 * @details Values are divided by 1024 and reported in KiB/s until they reach
 *          1 MiB/s, then in MiB/s. When no transfer is running the rate reads
 *          zero so a stale value is never shown.
 *
 * @param[in]  bytes_per_second Rate reported by the model.
 * @param[out] output           Buffer of 16 bytes receiving the text.
 */
static void rate_text(uint32_t bytes_per_second, char output[16])
{
	uint32_t kib = ui.model.running ? bytes_per_second / 1024U : 0U;

	snprintf(output, 16, "%u%s/s", kib >= 1024U ? kib / 1024U : kib, kib >= 1024U ? "M" : "K");
}

/**
 * @brief Return the link state text and its accent colour.
 *
 * @details Priority order: detecting, no card, not running, then running. A
 *          running link is green when the host configured it and amber while it
 *          still waits for the host.
 *
 * @param[out] color Receives the colour for the state chip.
 * @return Constant state text; never NULL.
 */
static const char *direct_state(uint32_t *color)
{
	if (ui.model.card_detecting) {
		*color = UI_WARNING;
		return "Identifying card";
	}
	if (!ui.model.card_present) {
		*color = UI_MUTED;
		return "No card detected";
	}
	if (!ui.model.running) {
		*color = UI_ACCENT;
		return "Ready to connect";
	}
	*color = ui.model.configured ? UI_SUCCESS : UI_WARNING;
	return ui.model.configured ? "Connected" : "Waiting for host";
}

/* Refresh only the live values, so a running transfer does not rebuild the page. */
/**
 * @brief Refresh the Direct page's live values in place.
 *
 * @details Updates the status chip colour and text, then formats both rates.
 *          It returns early when the page widgets are not built, so it is safe
 *          to call from the runtime task on any page. Only the labels and the
 *          chip colour are touched, so a running transfer does not rebuild the
 *          page.
 */
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
	snprintf(text, sizeof(text), "Read %s", rate);
	lv_label_set_text(ui.rate_read, text);
	rate_text(ui.model.write_rate, rate);
	snprintf(text, sizeof(text), "Write %s", rate);
	lv_label_set_text(ui.rate_write, text);
}

/* Flex row that spans the hero card, so every line starts on the same inner edge. */
/**
 * @brief Create a full-width flex row inside the hero card.
 *
 * @details The row spans the hero card so every line starts on the same inner
 *          edge, with children aligned to the end on the cross axis.
 *
 * @param[in] hero Hero card receiving the row.
 * @return The new row box.
 */
static lv_obj_t *make_hero_row(lv_obj_t *hero)
{
	lv_obj_t *row = make_box(hero, lv_pct(100), LV_SIZE_CONTENT);

	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	return row;
}

/**
 * @brief Build the Direct page widget tree.
 *
 * @details Creates the glass hero card and fills it top to bottom: the decimal
 *          capacity with its unit, the status chip, the read/write rate row,
 *          and the card capability chips (table type, partition count, and
 *          read/write mode, or an insert-card hint). The connect or safe-stop
 *          button is docked at the bottom and disabled while busy or while
 *          there is nothing to connect.
 */
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

	/* keep the chip label for the in-place refresh */
	ui.direct_status_chip = make_chip(hero, "", UI_MUTED);
	ui.direct_status = lv_obj_get_child(ui.direct_status_chip, 0);

	row = make_box(hero, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	/* both rates share the row at just under half width each */
	ui.rate_read = make_label(row, "", UI_TEXT);
	lv_obj_set_width(ui.rate_read, lv_pct(48));
	ui.rate_write = make_label(row, "", UI_TEXT);
	lv_obj_set_width(ui.rate_write, lv_pct(48));

	row = make_box(hero, lv_pct(100), 24);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 6, 0);
	if (ui.model.card_present) {
		make_chip(row, ui.model.table == BLK_PARTITION_TABLE_GPT ? "GPT" : "MBR", UI_MUTED);
		snprintf(text, sizeof(text), "%u parts", (uint32_t)ui.model.partition_count);
		make_chip(row, text, UI_MUTED);
		make_chip(row, ui.model.card_writable ? "RW" : "RO",
			ui.model.card_writable ? UI_SUCCESS : UI_WARNING);
	} else {
		make_chip(row, "Insert a card", UI_MUTED);
	}
	/* fill the live values before the first frame is shown */
	ui_update_direct();

	/* the primary action is docked at the bottom of the page */
	button = make_button(ui.content, ui.model.running ? "Stop safely" : "Connect USB", start_event, NULL);
	lv_obj_set_width(button, ui_wide_width());
	lv_obj_set_style_radius(button, APP_CARD_RADIUS, 0);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, 0);
	ui_glass_neutral(button);
	set_disabled(button, ui.model.busy || (!ui.model.running && !ui.model.card_present));
}
