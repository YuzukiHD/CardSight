// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_UI_INTERNAL_H
#define CARDSIGHT_UI_INTERNAL_H
#include "app/app.h"
#include "ui/ui.h"
#include "ui/backdrop.h"
#include "ui/icons.h"
#include "ui/glass.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include <gui/lvgl_port.h>
#include <hal/kernel.h>
#include <log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <usb/mass_storage.h>
#include <lvgl.h>
/*
 * The panel has rounded corners. Everything that touches an edge is inset from it and given the
 * corner radius minus that inset, so its outline stays concentric with the glass.
 */
#define APP_SCREEN_RADIUS 45
#define APP_BAR_MARGIN 10
#define APP_BAR_HEIGHT 50
#define APP_GAP 8
#define APP_EDGE_PAD 10
#define APP_CARD_RADIUS 16
#define APP_BUTTON_HEIGHT 46
#define APP_ROW_HEIGHT 44
#define APP_PART_ROW_HEIGHT 52
#define APP_TOUCH_MIN 44
#define APP_HEADER_HEIGHT 30
#define APP_TAB_COUNT 5U

enum cardsight_mode { CARDSIGHT_DIRECT, CARDSIGHT_PROXY, CARDSIGHT_MANAGE, CARDSIGHT_FILES, CARDSIGHT_INFO };
struct cardsight_ui {
	lv_obj_t *screen;
	lv_obj_t *shell;
	lv_obj_t *page_host;
	lv_obj_t *tiles[APP_TAB_COUNT];
	lv_obj_t *banner;
	lv_obj_t *banner_fill;
	lv_obj_t *banner_label;
	lv_obj_t *format_dialog;
	lv_obj_t *tabbar;
	lv_obj_t *tab_indicator;
	lv_obj_t *tab_indicator_backdrop;
	lv_obj_t *tab_buttons[APP_TAB_COUNT];
	lv_obj_t *tab_icons[APP_TAB_COUNT];
	lv_obj_t *bar;
	lv_obj_t *background;
	lv_obj_t *header_title;
	lv_obj_t *header_status;
	lv_obj_t *content;
	lv_obj_t *direct_status;
	lv_obj_t *direct_status_chip;
	lv_obj_t *rate_read;
	lv_obj_t *rate_write;
	lv_obj_t *proxy_start;
	lv_obj_t *layout_apply;
	lv_obj_t *calibration;
	int32_t screen_radius;
	uint16_t width;
	uint16_t height;
	bool landscape;
	bool rebuild_pending;
	bool selection_initialized;
	bool manage_detail;
	bool files_detail;
	bool error_acknowledged;
	bool format_dialog_open;
	enum blk_partition_content format_content;
	enum blk_partition_table_type layout_table;
	struct cardsight_state model;
	uint32_t selected;
	enum cardsight_mode mode;
	uint32_t manage_partition;
	uint32_t files_partition;
	char files_path[VFS_PATH_MAX];
	bool layout_editor;
	uint32_t layout_count;
	uint64_t layout_sectors;
	uint32_t layout_mib[APP_LAYOUT_MAX];
	enum blk_partition_content layout_content[APP_LAYOUT_MAX];
};
extern struct cardsight_ui ui;
void ui_schedule_rebuild(void);
void show_page(enum cardsight_mode mode);
void ui_update_tabs(void);
void ui_update_header(void);
void ui_update_backdrop(void);
void ui_back_event(lv_event_t *event);
void ui_open_calibration(int32_t radius);
int32_t ui_bar_radius(void);
void cardsight_ui_refresh(void *argument);
void cardsight_ui_runtime(void *argument);
void build_direct(void);
void ui_update_direct(void);
void ui_update_proxy_controls(void);
void build_proxy(void);
void build_manage(void);
void build_format_dialog(void);
void ui_update_progress(void);
void layout_open_event(lv_event_t *event);
void build_layout_editor(void);
void build_layout_bar(lv_obj_t *bar);
void build_info(void);
void build_files(void);
void files_go_back(void);
#endif
