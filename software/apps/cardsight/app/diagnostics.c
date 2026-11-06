// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"
#include "app/diagnostics.h"
#include <stdlib.h>
#ifdef CONFIG_SHELL
#include <shell.h>
#endif
#ifdef CONFIG_SHELL
static struct semaphore ui_command_done;
static int ui_command_result;
static enum cardsight_mode ui_requested_mode;
static bool ui_requested_layout;
static int ui_requested_radius;
#endif

static bool area_contains(const lv_area_t *outer, const lv_area_t *inner)
{
	return inner->x1 >= outer->x1 && inner->y1 >= outer->y1 &&
		inner->x2 <= outer->x2 && inner->y2 <= outer->y2;
}

/* Is the point inside the rounded glass? Only the four corner squares can fail. */
static bool point_on_glass(int32_t x_pos, int32_t y_pos)
{
	int32_t radius = ui.screen_radius;
	int32_t dx = x_pos < radius ? radius - x_pos : x_pos > (int32_t)ui.width - 1 - radius ?
		x_pos - ((int32_t)ui.width - 1 - radius) : 0;
	int32_t dy = y_pos < radius ? radius - y_pos : y_pos > (int32_t)ui.height - 1 - radius ?
		y_pos - ((int32_t)ui.height - 1 - radius) : 0;

	return dx * dx + dy * dy <= radius * radius;
}

/* A rounded object only reaches its bounding corner along the arc; test the 45 degree point of it. */
static bool corners_on_glass(lv_obj_t *object, const lv_area_t *area)
{
	int32_t radius = MIN(lv_obj_get_style_radius(object, LV_PART_MAIN),
		MIN(lv_area_get_width(area), lv_area_get_height(area)) / 2);
	int32_t inset = radius - radius * 7071 / 10000;

	return point_on_glass(area->x1 + inset, area->y1 + inset) &&
		point_on_glass(area->x2 - inset, area->y1 + inset) &&
		point_on_glass(area->x1 + inset, area->y2 - inset) &&
		point_on_glass(area->x2 - inset, area->y2 - inset);
}

static void validate_tree(lv_obj_t *parent, const char *page, bool clipped, int *failures)
{
	lv_area_t parent_area;
	bool scrollable = lv_obj_is_scrollable(parent);
	uint32_t children = lv_obj_get_child_count(parent);

	lv_obj_get_coords(parent, &parent_area);
	for (uint32_t index = 0U; index < children; index++) {
		lv_obj_t *child = lv_obj_get_child(parent, (int32_t)index);
		lv_area_t child_area;

		if (lv_obj_is_hidden(child)) {
			continue;
		}
		lv_obj_get_coords(child, &child_area);
		if (!scrollable && !area_contains(&parent_area, &child_area)) {
			pr_err("UI test %s: child %u outside parent (%ld,%ld)-(%ld,%ld)\n", page,
				(uint32_t)index, (long)child_area.x1, (long)child_area.y1,
				(long)child_area.x2, (long)child_area.y2);
			(*failures)++;
		}
		if (!clipped && !scrollable && !lv_obj_is_scrollable(child) && !corners_on_glass(child, &child_area)) {
			pr_err("UI test %s: child %u leaves the rounded glass r=%ld (%ld,%ld)-(%ld,%ld)\n", page,
				(uint32_t)index, (long)lv_obj_get_style_radius(child, LV_PART_MAIN), (long)child_area.x1, (long)child_area.y1,
				(long)child_area.x2, (long)child_area.y2);
			(*failures)++;
		}
		if (lv_obj_is_clickable(child) &&
			(lv_obj_get_width(child) < APP_TOUCH_MIN || lv_obj_get_height(child) < APP_TOUCH_MIN)) {
			pr_err("UI test %s: clickable child %u is %ldx%ld\n", page, (uint32_t)index,
				(long)lv_obj_get_width(child), (long)lv_obj_get_height(child));
			(*failures)++;
		}
		validate_tree(child, page, clipped || scrollable, failures);
	}
}

static void validate_object_tree(lv_obj_t *parent, const char *page, int *failures)
{
	validate_tree(parent, page, false, failures);
}

static void validate_active_page(const char *name, int *failures)
{
	lv_obj_t *active = ui.tiles[(uint32_t)ui.mode];

	lv_obj_update_layout(ui.screen);
	if (active == NULL || ui.bar == NULL) {
		pr_err("UI test %s: no active tile or bottom bar\n", name);
		(*failures)++;
		return;
	}
	validate_object_tree(active, name, failures);
	validate_object_tree(ui.bar, name, failures);
}

int cardsight_validate_ui(void)
{
	int failures = 0;
	validate_active_page("active", &failures);
	if (ui.format_dialog != NULL) validate_object_tree(ui.format_dialog, "format", &failures);
	if (ui.banner != NULL) validate_object_tree(ui.banner, "banner", &failures);
	return failures;
}

#ifdef CONFIG_SHELL
static void ui_select_page_callback(void *argument)
{
	bool rebuild;

	(void)argument;
	rebuild = ui.layout_editor != ui_requested_layout || ui.manage_detail;
	ui.manage_detail = false;
	ui.mode = ui_requested_mode;
	ui.layout_editor = ui_requested_layout;
	if (rebuild) {
		cardsight_ui_refresh(NULL);
	} else {
		show_page(ui_requested_mode);
	}
	ui_command_result = 0;
	up(&ui_command_done);
}

/* Shows the corner calibration overlay; touch it to adjust, tap the middle to accept. */
static void ui_radius_callback(void *argument)
{
	(void)argument;
	ui_open_calibration(ui_requested_radius != 0 ? ui_requested_radius : ui.screen_radius);
	ui_command_result = 0;
	up(&ui_command_done);
}

static void ui_selftest_callback(void *argument)
{
	static const char *names[] = { "direct", "proxy", "disk", "info" };
	enum cardsight_mode saved_mode;
	bool saved_layout;
	int failures = 0;

	(void)argument;
	saved_mode = ui.mode;
	saved_layout = ui.layout_editor;
	for (uint32_t index = 0U; index < ARRAY_SIZE(names); index++) {
		show_page((enum cardsight_mode)index);
		validate_active_page(names[index], &failures);
	}
	ui.mode = CARDSIGHT_MANAGE;
	ui.layout_editor = true;
	cardsight_ui_refresh(NULL);
	validate_active_page("layout", &failures);
	ui.mode = saved_mode;
	ui.layout_editor = saved_layout;
	cardsight_ui_refresh(NULL);
	ui_command_result = failures == 0 ? 0 : -EINVAL;
	up(&ui_command_done);
}

static int command_cardsight(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "usb-test") == 0) {
		const struct storage_proxy_config config = {
			.source_disk = CONFIG_STORAGE_PROXY_SOURCE_DISK,
			.selected_partitions = 1U << (STORAGE_PROXY_MAX_PARTITIONS - 1U),
		};
		int result;

		if (usb_mass_storage_running() || storage_proxy_running()) {
			return -EBUSY;
		}
		result = storage_proxy_start_config(&config);
		if (result == 0 && !storage_proxy_read_only()) {
			result = -EACCES;
		}
		if (result == 0) {
			result = usb_mass_storage_start_device(storage_proxy_device_name());
		}
		if (result == 0) {
			msleep(100);
			usb_mass_storage_stop();
		}
		storage_proxy_stop();
		printf("CardSight read-only USB lifecycle test: %s\n", result == 0 ? "PASS" : "FAIL");
		return result;
	}
	if (argc == 3 && strcmp(argv[1], "usb") == 0) {
		const struct storage_proxy_config config = {
			.source_disk = CONFIG_STORAGE_PROXY_SOURCE_DISK,
			.selected_partitions = 1U << (STORAGE_PROXY_MAX_PARTITIONS - 1U),
		};
		int result = 0;

		if (strcmp(argv[2], "off") == 0) {
			usb_mass_storage_stop();
			storage_proxy_stop();
			printf("USB mass storage stopped\n");
			return 0;
		}
		if (strcmp(argv[2], "disk") == 0) {
			/* the whole card, read-only: for host-side read tests of the real table */
			if (storage_proxy_running() || usb_mass_storage_running()) {
				return -EBUSY;
			}
			usb_mass_storage_force_read_only(true);
			result = usb_mass_storage_start_device(CONFIG_STORAGE_PROXY_SOURCE_DISK);
			printf("USB whole disk %s: %d\n", result == 0 ? "started" : "failed", result);
			return result;
		}
		if (strcmp(argv[2], "on") != 0) {
			return -EINVAL;
		}
		usb_mass_storage_force_read_only(false);
		if (!storage_proxy_running()) {
			result = storage_proxy_start_config(&config);
		}
		if (result == 0) {
			result = usb_mass_storage_start_device(storage_proxy_device_name());
		}
		printf("USB mass storage %s: %d\n", result == 0 ? "started" : "failed", result);
		return result;
	}
	if (argc == 2 && strcmp(argv[1], "ui-test") == 0) {
		if (lvgl_port_post(ui_selftest_callback, NULL) != 0 || down_timeout(&ui_command_done, 5000) != 0) {
			return -ETIMEDOUT;
		}
		printf("CardSight UI test: %s\n", ui_command_result == 0 ? "PASS" : "FAIL");
		return ui_command_result;
	}
	if (argc == 3 && strcmp(argv[1], "page") == 0) {
		ui_requested_layout = false;
		if (strcmp(argv[2], "direct") == 0) {
			ui_requested_mode = CARDSIGHT_DIRECT;
		} else if (strcmp(argv[2], "proxy") == 0) {
			ui_requested_mode = CARDSIGHT_PROXY;
		} else if (strcmp(argv[2], "disk") == 0) {
			ui_requested_mode = CARDSIGHT_MANAGE;
		} else if (strcmp(argv[2], "info") == 0) {
			ui_requested_mode = CARDSIGHT_INFO;
		} else if (strcmp(argv[2], "layout") == 0) {
			ui_requested_mode = CARDSIGHT_MANAGE;
			ui_requested_layout = true;
		} else {
			return -EINVAL;
		}
		if (lvgl_port_post(ui_select_page_callback, NULL) != 0 || down_timeout(&ui_command_done, 5000) != 0) {
			return -ETIMEDOUT;
		}
		return ui_command_result;
	}
	if (argc >= 2 && strcmp(argv[1], "radius") == 0) {
		ui_requested_radius = argc == 3 ? atoi(argv[2]) : 0;
		if (lvgl_port_post(ui_radius_callback, NULL) != 0 || down_timeout(&ui_command_done, 5000) != 0) {
			return -ETIMEDOUT;
		}
		return ui_command_result;
	}
	printf("cardsight page <direct|proxy|disk|info|layout> | radius [px] | ui-test | usb-test | usb <on|off|disk>\n");
	return -EINVAL;
}

SHELL_CMD(cardsight, "cardsight page <name> | radius [px] | ui-test | usb-test: board UI diagnostics", command_cardsight);
#endif

void cardsight_diagnostics_init(void)
{
#ifdef CONFIG_SHELL
	sema_init_limit(&ui_command_done, 0U, 1U);
#endif
}
