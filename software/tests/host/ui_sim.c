// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PC renderer for the CardSight UI: the real LVGL, pages and glass material are built against
 * small host stubs, rendered to an in-memory RGB565 frame and written as PPM for inspection.
 */

#include "ui/ui_internal.h"

#include <bits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_WIDTH  240
#define SIM_HEIGHT 284

static struct cardsight_state model;
static uint8_t framebuffer[SIM_WIDTH * SIM_HEIGHT * 3U] __attribute__((aligned(64)));
static uint32_t tick_ms;

static uint32_t sim_tick(void)
{
	return tick_ms;
}

void log_print(int level, const char *module, const char *format, ...)
{
	va_list arguments;

	(void)level;
	(void)module;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

void cardsight_snapshot(struct cardsight_state *state)
{
	*state = model;
}

void send_request(const struct cardsight_request *request)
{
	(void)request;
}

int sunxi_display_get_info(struct sunxi_display_info *info)
{
	memset(info, 0, sizeof(*info));
	info->width = SIM_WIDTH;
	info->height = SIM_HEIGHT;
	info->fb_width = SIM_WIDTH;
	info->fb_height = SIM_HEIGHT;
	info->stride = SIM_WIDTH * 2U;
	info->bytes_per_pixel = 2U;
	info->pixels_swapped = true;
	return 0;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static void fixture(void)
{
	static const char *const types[] = { "FAT32", "exFAT", "ext4" };
	static const char *const labels[] = { "BOOT", "PHOTOS", "LINUX" };
	static const struct {
		const char *name;
		uint64_t size;
		bool dir;
	} files[] = {
		{ "文件文件文件文件文件文件文件文件文件文件文件文件.txt", 88123U, false },
		{ "DCIM", 0U, true },
		{ "Music", 0U, true },
		{ "System Volume Information", 0U, true },
		{ "hello.txt", 11U, false },
		{ "IMG_20261106_123456.jpg", 4200000U, false },
		{ "README.md", 1234U, false },
	};

	memset(&model, 0, sizeof(model));
	model.card_present = true;
	model.card_writable = true;
	model.card_info_valid = true;
	model.sectors = 62333952U;
	model.partition_count = 3U;
	model.table = BLK_PARTITION_TABLE_MBR;
	snprintf(model.card.identity.product_name, sizeof(model.card.identity.product_name), "SK32G");
	model.card.mode = "SDR104";
	model.card.clock = 200000000U;
	model.card.bus_width = 4U;
	model.card.tuned = true;
	model.card.high_capacity = true;
	for (uint32_t index = 0U; index < model.partition_count; index++) {
		struct storage_proxy_partition_info *part = &model.partitions[index];

		part->partno = index + 1U;
		part->sectors = 20000000U;
		part->present = true;
		part->selected = true;
		part->writable = index != 2U;
		part->status = 0;
		snprintf(part->device, sizeof(part->device), "mmcblk0p%u", index + 1U);
		snprintf(part->volume.fstype, sizeof(part->volume.fstype), "%s", types[index]);
		snprintf(part->volume.label, sizeof(part->volume.label), "%s", labels[index]);
		part->volume.free_bytes = 12000000000ULL;
	}
	model.files.valid = true;
	model.files.partition = 2U;
	snprintf(model.files.path, sizeof(model.files.path), "/");
	model.files.count = ARRAY_SIZE(files);
	model.files.total = ARRAY_SIZE(files);
	for (uint32_t index = 0U; index < ARRAY_SIZE(files); index++) {
		snprintf(model.files.entries[index].name, sizeof(model.files.entries[index].name), "%s",
			files[index].name);
		model.files.entries[index].size = files[index].size;
		model.files.entries[index].dir = files[index].dir;
	}
}

static void save_ppm(const char *path)
{
	static uint8_t row[SIM_WIDTH * 3U];
	FILE *file = fopen(path, "wb");

	if (file == NULL) {
		perror(path);
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", SIM_WIDTH, SIM_HEIGHT);
	for (uint32_t y = 0U; y < SIM_HEIGHT; y++) {
		const uint8_t *source = framebuffer + (size_t)y * SIM_WIDTH * 3U;

		for (uint32_t x = 0U; x < SIM_WIDTH; x++) {
			row[x * 3U] = source[x * 3U + 2U];
			row[x * 3U + 1U] = source[x * 3U + 1U];
			row[x * 3U + 2U] = source[x * 3U];
		}
		fwrite(row, sizeof(row), 1U, file);
	}
	fclose(file);
	printf("wrote %s\n", path);
}

int main(void)
{
	static const struct {
		const char *name;
		enum cardsight_mode mode;
		bool files_detail;
	} scenes[] = {
		{ "direct", CARDSIGHT_DIRECT, false },
		{ "proxy", CARDSIGHT_PROXY, false },
		{ "manage", CARDSIGHT_MANAGE, false },
		{ "files", CARDSIGHT_FILES, false },
		{ "files-list", CARDSIGHT_FILES, true },
		{ "info", CARDSIGHT_INFO, false },
	};
	lv_display_t *display;

	lv_init();
	lv_tick_set_cb(sim_tick);
	display = lv_display_create(SIM_WIDTH, SIM_HEIGHT);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
	lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer),
		LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(display, flush);
	fixture();


	for (uint32_t index = 0U; index < ARRAY_SIZE(scenes); index++) {
		char path[64];

		ui.files_detail = scenes[index].files_detail;
		if (scenes[index].files_detail) {
			ui.files_partition = 2U;
			snprintf(ui.files_path, sizeof(ui.files_path), "/");
		}
		cardsight_ui_refresh(NULL);
		show_page(scenes[index].mode);
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(display);
		snprintf(path, sizeof(path), "/tmp/ui-%s.ppm", scenes[index].name);
		save_ppm(path);

	}
	/* a failed card read opens the error sheet */
	{
		model.error = -EIO;
		model.request = CARDSIGHT_REQUEST_SCAN;
		snprintf(model.operation, sizeof(model.operation), "%s", "存储卡读写错误");
		snprintf(model.card_error, sizeof(model.card_error), "%s", "CMD5 Read Timeout");
		ui.dismissed_error = 0;
		cardsight_ui_refresh(NULL);
		for (uint32_t step = 0U; step < 5U; step++) {
			tick_ms += 10U;
			lv_timer_handler();
		}
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(display);
		save_ppm("/tmp/ui-error.ppm");
		model.error = 0;
		model.card_error[0] = '\0';
		model.request = CARDSIGHT_REQUEST_SCAN;
	}
	/* a card is being identified */
	{
		model.card_detecting = true;
		model.card_present = false;
		cardsight_ui_refresh(NULL);
		for (uint32_t step = 0U; step < 5U; step++) {
			tick_ms += 10U;
			lv_timer_handler();
		}
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(display);
		save_ppm("/tmp/ui-detecting.ppm");
		model.card_detecting = false;
	}
	/* files: enter a directory, then come straight back */
	{
		ui.files_detail = true;
		ui.files_partition = 2U;
		snprintf(ui.files_path, sizeof(ui.files_path), "/");
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_FILES);
		lv_refr_now(display);
		files_go_back();
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_FILES);
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(display);
		save_ppm("/tmp/ui-files-back.ppm");
	}
	/* pressed / checked states, to prove the theme cannot draw a box */
	{
		lv_obj_t *button = NULL;

		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_DIRECT);
		for (uint32_t child = 0U; child < lv_obj_get_child_count(ui.tiles[CARDSIGHT_DIRECT]); child++) {
			lv_obj_t *item = lv_obj_get_child(ui.tiles[CARDSIGHT_DIRECT], (int32_t)child);

			if (lv_obj_check_type(item, &lv_button_class)) {
				button = item;
				break;
			}
		}
		if (button != NULL) {
			lv_obj_add_state(button, LV_STATE_PRESSED);
			printf("pressed bg_opa=%d bg_color=%06lx opa=%d image=%p\n",
				(int)lv_obj_get_style_bg_opa(button, 0),
				(unsigned long)lv_color_to_u32(lv_obj_get_style_bg_color(button, 0)) & 0xffffffUL,
				(int)lv_obj_get_style_opa(button, 0),
				(void *)lv_obj_get_style_bg_image_src(button, 0));
			printf("pressed state=%lx outline=%d border=%d side=%d shadow=%d radius=%d\n",
				(unsigned long)lv_obj_get_state(button),
				(int)lv_obj_get_style_outline_width(button, 0),
				(int)lv_obj_get_style_border_width(button, 0),
				(int)lv_obj_get_style_border_side(button, 0),
				(int)lv_obj_get_style_shadow_width(button, 0),
				(int)lv_obj_get_style_radius(button, 0));
		}
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(display);
		save_ppm("/tmp/ui-pressed.ppm");
	}

	/* the partition layout editor */
	ui.mode = CARDSIGHT_MANAGE;
	ui.layout_editor = true;
	ui.layout_table = BLK_PARTITION_TABLE_MBR;
	ui.layout_content[0] = BLK_PARTITION_EXFAT;
	cardsight_ui_refresh(NULL);
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(display);
	save_ppm("/tmp/ui-layout.ppm");
	ui.layout_editor = false;

	/* show the info page scrolled, to check that rows stop below the header */
	ui.files_detail = false;
	cardsight_ui_refresh(NULL);
	show_page(CARDSIGHT_INFO);
	{
		lv_obj_t *tile = ui.tiles[CARDSIGHT_INFO];

		for (uint32_t child = 0U; child < lv_obj_get_child_count(tile); child++) {
			lv_obj_t *item = lv_obj_get_child(tile, (int32_t)child);

			if (lv_obj_is_scrollable(item)) {
				lv_obj_scroll_to_y(item, 90, LV_ANIM_OFF);
				break;
			}
		}
	}
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(display);
	save_ppm("/tmp/ui-info-scroll.ppm");

	/* proxy partition selection: the same row clicked three times, rendered after each */
	{
		lv_obj_t *row = NULL;

		ui.files_detail = false;
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_PROXY);
		for (uint32_t child = 0U; child < lv_obj_get_child_count(ui.tiles[CARDSIGHT_PROXY]); child++) {
			lv_obj_t *item = lv_obj_get_child(ui.tiles[CARDSIGHT_PROXY], (int32_t)child);

			if (!lv_obj_is_scrollable(item)) {
				continue;
			}
			for (uint32_t index = 0U; index < lv_obj_get_child_count(item); index++) {
				lv_obj_t *entry = lv_obj_get_child(item, (int32_t)index);

				if (lv_obj_is_clickable(entry)) {
					row = entry;
					break;
				}
			}
			if (row != NULL) {
				break;
			}
		}
		if (row != NULL) {
			lv_obj_invalidate(lv_screen_active());
			lv_refr_now(display);
			save_ppm("/tmp/ui-proxy-sel0.ppm");
			printf("proxy sel0: checked=%d border=%d bg=%d selected=%08lx\n",
				(int)lv_obj_has_state(row, LV_STATE_CHECKED),
				(int)lv_obj_get_style_border_width(row, LV_PART_MAIN),
				(int)lv_obj_get_style_bg_opa(row, LV_PART_MAIN), (unsigned long)ui.selected);
			for (uint32_t step = 1U; step <= 3U; step++) {
				char path[64];

				lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
				lv_obj_invalidate(lv_screen_active());
				lv_refr_now(display);
				snprintf(path, sizeof(path), "/tmp/ui-proxy-sel%u.ppm", (uint32_t)step);
				save_ppm(path);
				printf("proxy sel%u: checked=%d border=%d bg=%d selected=%08lx\n",
					(uint32_t)step, (int)lv_obj_has_state(row, LV_STATE_CHECKED),
					(int)lv_obj_get_style_border_width(row, LV_PART_MAIN),
					(int)lv_obj_get_style_bg_opa(row, LV_PART_MAIN),
					(unsigned long)ui.selected);
			}
			/* what the board may be doing: a busy model disables the rows */
			lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
			set_disabled(row, true);
			lv_obj_invalidate(lv_screen_active());
			lv_refr_now(display);
			save_ppm("/tmp/ui-proxy-disabled.ppm");
			printf("proxy disabled: border=%d side=%d bg=%d opa=%d\n",
				(int)lv_obj_get_style_border_width(row, LV_PART_MAIN),
				(int)lv_obj_get_style_border_side(row, LV_PART_MAIN),
				(int)lv_obj_get_style_bg_opa(row, LV_PART_MAIN),
				(int)lv_obj_get_style_opa(row, LV_PART_MAIN));
		}
	}
	return 0;
}
