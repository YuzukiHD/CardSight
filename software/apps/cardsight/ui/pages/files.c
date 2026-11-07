// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The file browser: first the partitions of the card, then one directory at a time. The worker
 * lists the directory, so the UI never waits on the card; this page only renders the snapshot.
 */

#include "ui/ui_internal.h"

#include <size.h>

#define FILES_HEADER_HEIGHT 28
#define FILES_ROW_INSET 14
#define FILES_SIZE_WIDTH 64

/* Width of a row inside the scrolling list: the list has APP_EDGE_PAD on both sides. */
static int32_t row_width(void)
{
	return (int32_t)ui.width - 2 * APP_EDGE_PAD;
}

static void list_request(void)
{
	struct cardsight_request request = {
		.type = CARDSIGHT_REQUEST_LIST,
		.partition = ui.files_partition,
	};

	/* Pulling the card while the browser is open must not turn into "card not found"
	 * errors; there is simply nothing to list. */
	if (!ui.model.card_present) {
		return;
	}
	snprintf(request.path, sizeof(request.path), "%s", ui.files_path);
	send_request(&request);
}

static bool list_matches(void)
{
	return ui.model.files.valid && ui.model.files.partition == ui.files_partition &&
		strcmp(ui.model.files.path, ui.files_path) == 0;
}

static const char *list_error_text(int error)
{
	switch (error) {
	case -ENODEV: return "无法打开分区";
	case -ENOENT: return "目录不存在";
	case -ENAMETOOLONG: return "路径过长";
	case -ENOMEM: return "内存不足";
	default: return "读取失败";
	}
}

/* ---- the partition list ---- */

static void partition_event(lv_event_t *event)
{
	ui.files_partition = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
	snprintf(ui.files_path, sizeof(ui.files_path), "/");
	ui.files_detail = true;
	list_request();
	ui_schedule_rebuild();
}

static void build_partition_rows(lv_obj_t *list)
{
	for (size_t index = 0U; index < ui.model.partition_count; index++) {
		const struct storage_proxy_partition_info *part = &ui.model.partitions[index];
		char capacity[24];
		char text[64];
		lv_obj_t *button = lv_button_create(list);
		lv_obj_t *dot = make_card(button, 10, 10,
			part->status == 0 ? fstype_color(part->volume.fstype) : UI_MUTED);
		lv_obj_t *label;

		capacity_text(part->sectors, capacity, sizeof(capacity));
		lv_obj_set_size(button, lv_pct(100), APP_PART_ROW_HEIGHT);
		ui_style_button(button, UI_BUTTON_NORMAL);
		lv_obj_set_style_radius(button, 22, 0);
		disable_scroll(button);
		lv_obj_set_style_radius(dot, 5, 0);
		lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
		snprintf(text, sizeof(text), "P%u  %s", part->partno,
			part->status == 0 ? part->volume.fstype : "无法识别");
		label = make_label(button, text, UI_TEXT);
		lv_obj_align(label, LV_ALIGN_TOP_LEFT, 38, 1);
		snprintf(text, sizeof(text), "%s%s", capacity, part->writable ? "" : " 只读");
		label = make_label(button, text, part->status == 0 ? UI_MUTED : UI_WARNING);
		lv_obj_set_style_text_font(label, &cardsight_font_16, 0);
		lv_obj_align(label, LV_ALIGN_TOP_LEFT, 38, 28);
		if (part->status == 0) {
			lv_obj_add_event_cb(button, partition_event, LV_EVENT_CLICKED,
				(void *)(uintptr_t)part->partno);
		}
		set_disabled(button, ui.model.busy || part->status != 0);
	}
	if (ui.model.partition_count == 0U) {
		make_label(list, ui.model.card_present ? "无可用分区" : "请插入存储卡", UI_MUTED);
	}
}

static void build_partitions(void)
{
	lv_obj_t *list;

	make_label(ui.content, "选择分区", UI_MUTED);
	list = make_list(FILES_HEADER_HEIGHT, 0);
	build_partition_rows(list);
}

/* ---- one directory ---- */

static void directory_event(lv_event_t *event)
{
	uint32_t index = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
	const struct cardsight_file_entry *entry = &ui.model.files.entries[index];
	size_t used = strcmp(ui.files_path, "/") == 0 ? 0U : strlen(ui.files_path);
	int written;

	written = snprintf(ui.files_path + used, sizeof(ui.files_path) - used, "/%s", entry->name);
	if (written < 0 || (size_t)written >= sizeof(ui.files_path) - used) {
		ui.files_path[used] = '\0';
		return;
	}
	list_request();
	ui_schedule_rebuild();
}

void files_go_back(void)
{
	char *slash;

	if (!ui.files_detail) {
		return;
	}
	if (strcmp(ui.files_path, "/") == 0) {
		ui.files_detail = false;
		ui_schedule_rebuild();
		return;
	}
	slash = strrchr(ui.files_path, '/');
	if (slash == NULL || slash == ui.files_path) {
		strcpy(ui.files_path, "/");
	} else {
		*slash = '\0';
	}
	list_request();
	ui_schedule_rebuild();
}

static void build_parent_row(lv_obj_t *list)
{
	lv_obj_t *button = make_button(list, "..", ui_back_event, NULL);

	lv_obj_set_size(button, lv_pct(100), APP_ROW_HEIGHT);
	lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
	ui_style_button(button, UI_BUTTON_GHOST);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_MUTED), 0);
	set_disabled(button, ui.model.busy);
}

static void build_directory_row(lv_obj_t *list, uint32_t index, const struct cardsight_file_entry *entry)
{
	lv_obj_t *button = lv_button_create(list);
	lv_obj_t *icon = lv_image_create(button);
	lv_obj_t *label;

	lv_obj_set_size(button, lv_pct(100), APP_ROW_HEIGHT);
	ui_style_button(button, UI_BUTTON_NORMAL);
	lv_obj_set_style_radius(button, APP_ROW_HEIGHT / 2, 0);
	disable_scroll(button);
	lv_image_set_src(icon, &cardsight_icon_folder);
	lv_obj_set_style_image_recolor(icon, lv_color_hex(UI_ACCENT), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_align(icon, LV_ALIGN_LEFT_MID, FILES_ROW_INSET, 0);
	label = make_label(button, entry->name, UI_TEXT);
	lv_obj_set_width(label, row_width() - FILES_ROW_INSET - 32 - FILES_ROW_INSET);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	ui_label_set_ellipsis(label, entry->name, row_width() - FILES_ROW_INSET - 32 - FILES_ROW_INSET);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, FILES_ROW_INSET + 32, 0);
	lv_obj_add_event_cb(button, directory_event, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
	set_disabled(button, ui.model.busy);
}

static void build_file_row(lv_obj_t *list, const struct cardsight_file_entry *entry)
{
	lv_obj_t *row = make_box(list, lv_pct(100), APP_ROW_HEIGHT);
	lv_obj_t *name = make_label(row, entry->name, UI_TEXT);
	lv_obj_t *size = make_label(row, "", UI_MUTED);
	char text[24];

	lv_obj_set_width(name, row_width() - 2 * FILES_ROW_INSET - APP_GAP - FILES_SIZE_WIDTH);
	lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_CLIP);
	ui_label_set_ellipsis(name, entry->name,
		row_width() - 2 * FILES_ROW_INSET - APP_GAP - FILES_SIZE_WIDTH);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, FILES_ROW_INSET, 0);
	format_capacity(entry->size, text, sizeof(text));
	lv_label_set_text(size, text);
	lv_obj_set_width(size, FILES_SIZE_WIDTH);
	lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_align(size, LV_ALIGN_RIGHT_MID, -FILES_ROW_INSET, 0);
}

static void build_directory_rows(lv_obj_t *list)
{
	if (strcmp(ui.files_path, "/") != 0) {
		build_parent_row(list);
	}
	for (size_t index = 0U; index < ui.model.files.count; index++) {
		const struct cardsight_file_entry *entry = &ui.model.files.entries[index];

		if (entry->dir) {
			build_directory_row(list, (uint32_t)index, entry);
		} else {
			build_file_row(list, entry);
		}
	}
}

static void build_directory(void)
{
	char head[VFS_PATH_MAX + 24];
	lv_obj_t *label;
	lv_obj_t *list;

	snprintf(head, sizeof(head), "P%u  %s", ui.files_partition, ui.files_path);
	label = make_label(ui.content, head, UI_MUTED);
	lv_obj_set_width(label, ui_page_width() - 8);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	ui_label_set_ellipsis(label, head, ui_page_width() - 8);
	list = make_list(FILES_HEADER_HEIGHT, 0);
	if (!list_matches()) {
		make_label(list, ui.model.busy ? "正在读取…" : "读取失败", UI_MUTED);
		return;
	}
	if (ui.model.files.error != 0) {
		make_label(list, list_error_text(ui.model.files.error), UI_DANGER);
		return;
	}
	if (ui.model.files.count == 0U) {
		make_label(list, "空目录", UI_MUTED);
		return;
	}
	build_directory_rows(list);
}

void build_files(void)
{
	if (ui.files_detail) {
		build_directory();
	} else {
		build_partitions();
	}
}
