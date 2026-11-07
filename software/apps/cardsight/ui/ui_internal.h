// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_internal.h
 * @brief Widget tree, layout metrics, and page entry points private to the UI.
 *
 * The panel has rounded corners. Everything that touches an edge is inset from
 * it and given the corner radius minus that inset, so its outline stays
 * concentric with the glass.
 */
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
/** @brief Corner radius of the physical display in pixels. */
#define APP_SCREEN_RADIUS 45
/** @brief Inset of the top app bar from the screen edge in pixels. */
#define APP_BAR_MARGIN 10
/** @brief Height of the top app bar in pixels. */
#define APP_BAR_HEIGHT 50
/** @brief Gap between stacked page elements in pixels. */
#define APP_GAP 8
/** @brief Extra padding at the screen edges in pixels. */
#define APP_EDGE_PAD 10
/** @brief Corner radius of content cards in pixels. */
#define APP_CARD_RADIUS 16
/** @brief Standard action-button height in pixels. */
#define APP_BUTTON_HEIGHT 46
/** @brief Height of a regular list row in pixels. */
#define APP_ROW_HEIGHT 44
/** @brief Height of a partition list row in pixels. */
#define APP_PART_ROW_HEIGHT 52
/** @brief Minimum touch target size in pixels. */
#define APP_TOUCH_MIN 44
/** @brief Height of a page section header in pixels. */
#define APP_HEADER_HEIGHT 30
/** @brief Number of tabs in the bottom tab bar. */
#define APP_TAB_COUNT 5U

/**
 * @brief Pages selectable from the tab bar.
 *
 * @details The values are ordered like the tabs: CARDSIGHT_DIRECT is the USB
 *          direct page, CARDSIGHT_PROXY the proxy page, CARDSIGHT_MANAGE the
 *          partition-management page, CARDSIGHT_FILES the file browser, and
 *          CARDSIGHT_INFO the card information page. The value doubles as the
 *          index into @c tiles.
 */
enum cardsight_mode { CARDSIGHT_DIRECT, CARDSIGHT_PROXY, CARDSIGHT_MANAGE, CARDSIGHT_FILES, CARDSIGHT_INFO };
/**
 * @brief Complete state of the live UI, including the widget tree and view model.
 *
 * One instance, @c ui, is shared by every page builder and event handler. It
 * caches widget pointers that the refresh path updates in place instead of
 * rebuilding the tree, which keeps scrolling and animations smooth.
 */
struct cardsight_ui {
	/** @brief Root screen object of the LVGL display. */
	lv_obj_t *screen;
	/** @brief Outer shell that clips the whole UI to the screen radius. */
	lv_obj_t *shell;
	/** @brief Container that hosts the currently visible page. */
	lv_obj_t *page_host;
	/** @brief Per-tab content tiles inside the page host. */
	lv_obj_t *tiles[APP_TAB_COUNT];
	/** @brief Banner container for transient status messages. */
	lv_obj_t *banner;
	/** @brief Progress fill of the banner, showing operation completion. */
	lv_obj_t *banner_fill;
	/** @brief Text label inside the banner. */
	lv_obj_t *banner_label;
	/** @brief Confirmation dialog for the layout/format operation. */
	lv_obj_t *format_dialog;
	/** @brief Dialog exposing the card settings. */
	lv_obj_t *settings_dialog;
	/** @brief Dialog that reports the last operation error. */
	lv_obj_t *error_dialog;
	/** @brief Overlay shown while the card is being detected. */
	lv_obj_t *detecting_card;
	/** @brief Bottom tab bar container. */
	lv_obj_t *tabbar;
	/** @brief Sliding indicator under the selected tab. */
	lv_obj_t *tab_indicator;
	/** @brief Glass backdrop child of the tab indicator. */
	lv_obj_t *tab_indicator_backdrop;
	/** @brief Button of each tab in the tab bar. */
	lv_obj_t *tab_buttons[APP_TAB_COUNT];
	/** @brief Icon of each tab in the tab bar. */
	lv_obj_t *tab_icons[APP_TAB_COUNT];
	/** @brief Bottom action bar that hosts page-specific buttons. */
	lv_obj_t *bar;
	/** @brief Wallpaper image behind the whole UI. */
	lv_obj_t *background;
	/** @brief Page title shown in the app bar. */
	lv_obj_t *header_title;
	/** @brief Item count shown at the right of the app bar. */
	lv_obj_t *header_count;
	/** @brief Connection/status text shown in the app bar. */
	lv_obj_t *header_status;
	/** @brief Scrollable content container of the page. */
	lv_obj_t *content;
	/** @brief Direct page: current link state label. */
	lv_obj_t *direct_status;
	/** @brief Direct page: chip behind the link state label. */
	lv_obj_t *direct_status_chip;
	/** @brief Direct page: read-rate label. */
	lv_obj_t *rate_read;
	/** @brief Direct page: write-rate label. */
	lv_obj_t *rate_write;
	/** @brief Proxy page: start/stop button. */
	lv_obj_t *proxy_start;
	/** @brief Layout editor: apply button. */
	lv_obj_t *layout_apply;
	/** @brief Calibration overlay/object used for touch calibration. */
	lv_obj_t *calibration;
	/** @brief Effective screen corner radius of the current orientation. */
	int32_t screen_radius;
	/** @brief Display width in pixels. */
	uint16_t width;
	/** @brief Display height in pixels. */
	uint16_t height;
	/** @brief True while the display is wider than it is tall. */
	bool landscape;
	/** @brief True when a deferred tree rebuild has been scheduled. */
	bool rebuild_pending;
	/** @brief True once the tab selection has been restored at least once. */
	bool selection_initialized;
	/** @brief True while the Manage page shows a partition detail view. */
	bool manage_detail;
	/** @brief True while the Files page shows a directory listing. */
	bool files_detail;
	/** @brief Error code the user dismissed, suppressed until it changes. */
	int dismissed_error;
	/** @brief True while the format/layout confirmation dialog is open. */
	bool format_dialog_open;
	/** @brief True when LVGL animations are enabled. */
	bool animations;
	/** @brief Content type chosen for the pending format operation. */
	enum blk_partition_content format_content;
	/** @brief Partition table type chosen in the layout editor. */
	enum blk_partition_table_type layout_table;
	/** @brief Latest snapshot of the application model pushed to the UI. */
	struct cardsight_state model;
	/** @brief Index of the currently selected partition or row. */
	uint32_t selected;
	/** @brief Currently visible page. */
	enum cardsight_mode mode;
	/** @brief Partition opened in the Manage page detail view. */
	uint32_t manage_partition;
	/** @brief Partition opened in the Files page. */
	uint32_t files_partition;
	/** @brief Directory path shown by the Files page. */
	char files_path[VFS_PATH_MAX];
	/** @brief True while the layout editor is open instead of the partition list. */
	bool layout_editor;
	/** @brief Number of partitions in the pending layout. */
	uint32_t layout_count;
	/** @brief Total sectors covered by the pending layout. */
	uint64_t layout_sectors;
	/** @brief Size in MiB of each pending layout partition. */
	uint32_t layout_mib[APP_LAYOUT_MAX];
	/** @brief Content type of each pending layout partition. */
	enum blk_partition_content layout_content[APP_LAYOUT_MAX];
};
/** @brief The single live UI instance; only the LVGL task mutates it. */
extern struct cardsight_ui ui;
/**
 * @brief Request a deferred rebuild of the whole widget tree on the LVGL task.
 */
void ui_schedule_rebuild(void);
/**
 * @brief Switch the visible page and update the tab bar.
 *
 * @param[in] mode Page to show.
 */
void show_page(enum cardsight_mode mode);
/**
 * @brief Refresh the tab bar selection and indicator from @c ui.mode.
 */
void ui_update_tabs(void);
/**
 * @brief Refresh the app bar title, count, and status text.
 */
void ui_update_header(void);
/**
 * @brief Recompute the wallpaper and glass objects after a layout change.
 */
void ui_update_backdrop(void);
/**
 * @brief Common handler for the back gesture or button.
 *
 * @param[in] event LVGL event that triggered the back action.
 */
void ui_back_event(lv_event_t *event);
/**
 * @brief Open the touch calibration overlay with a given screen radius.
 *
 * @param[in] radius Corner radius of the overlay in pixels.
 */
void ui_open_calibration(int32_t radius);
/**
 * @brief Return the corner radius used by the bottom action bar.
 *
 * @return Radius in pixels.
 */
int32_t ui_bar_radius(void);
/**
 * @brief Refresh task body: wait for change notifications and request redraws.
 *
 * @param[in] argument Unused task argument.
 */
void cardsight_ui_refresh(void *argument);
/**
 * @brief LVGL runtime task body: initialise the display and run the LVGL loop.
 *
 * @param[in] argument Unused task argument.
 */
void cardsight_ui_runtime(void *argument);
/**
 * @brief Build the Direct page widget tree.
 */
void build_direct(void);
/**
 * @brief Refresh the Direct page labels from the current model.
 */
void ui_update_direct(void);
/**
 * @brief Refresh the Proxy page start/stop button state.
 */
void ui_update_proxy_controls(void);
/**
 * @brief Build the Proxy page widget tree.
 */
void build_proxy(void);
/**
 * @brief Build the Manage page widget tree, either the list or a detail view.
 */
void build_manage(void);
/**
 * @brief Build the format/layout confirmation dialog.
 */
void build_format_dialog(void);
/**
 * @brief Refresh the progress banner from the active operation state.
 */
void ui_update_progress(void);
/**
 * @brief Open the layout editor from the partition list.
 *
 * @param[in] event LVGL click event carrying the selected partition.
 */
void layout_open_event(lv_event_t *event);
/**
 * @brief Build the partition layout editor page.
 */
void build_layout_editor(void);
/**
 * @brief Build the bottom action bar for the layout editor.
 *
 * @param[in] bar Bottom bar container receiving the buttons.
 */
void build_layout_bar(lv_obj_t *bar);
/**
 * @brief Build the Info page widget tree.
 */
void build_info(void);
/**
 * @brief Build the Files page widget tree for the current path.
 */
void build_files(void);
/**
 * @brief Leave the current directory on the Files page.
 */
void files_go_back(void);
#endif
