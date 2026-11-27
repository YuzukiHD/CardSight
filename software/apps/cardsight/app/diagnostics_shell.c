// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file diagnostics_shell.c
 * @brief The cardsight shell command and the LVGL callbacks it posts.
 *
 * @details The command runs UI work on the LVGL task through @c lvgl_port_post
 * and waits on a semaphore for it to finish, and it can start or stop USB mass
 * storage directly for host-side tests.
 */
#include "ui/ui_private.h"
#include "app/diagnostics.h"
#include <malloc.h>
#include <stdlib.h>
#ifdef CONFIG_SHELL
#include <shell.h>
#endif

#ifdef CONFIG_SHELL
/** @brief Released by an LVGL callback when the command it ran has finished. */
static struct semaphore ui_command_done;
/** @brief Result the last LVGL callback stored for the waiting command. */
static int ui_command_result;
/** @brief Page the shell asked the UI to show. */
static enum cardsight_mode ui_requested_mode;
/** @brief True when the shell asked for the layout editor instead of the plain page. */
static bool ui_requested_layout;
/** @brief Screen radius requested by the radius command, or zero for the current one. */
static int ui_requested_radius;

/**
 * @brief LVGL callback that switches to the page the shell selected.
 *
 * @details The UI is rebuilt when the layout editor or the manage detail state
 * changes, because those alter the page structure; otherwise only the tile is
 * shown. The layout editor detail is cleared after the choice, and the waiting
 * command is released with success.
 *
 * @param[in] argument Unused.
 */
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

/**
 * @brief LVGL callback that opens the corner calibration overlay.
 *
 * @details Shows the corner calibration overlay; touch it to adjust, tap the
 * middle to accept. The radius from the command is used when one was given,
 * otherwise the current screen radius.
 *
 * @param[in] argument Unused.
 */
static void ui_radius_callback(void *argument)
{
	(void)argument;
	ui_open_calibration(ui_requested_radius != 0 ? ui_requested_radius : ui.screen_radius);
	ui_command_result = 0;
	up(&ui_command_done);
}

/**
 * @brief LVGL callback that validates every page of the UI.
 *
 * @details Each page is shown in turn and validated, then the layout editor is
 * built and validated too. The previous page and editor flag are restored and
 * the UI rebuilt before the command is released, so the test leaves no trace.
 *
 * @param[in] argument Unused.
 */
static void ui_selftest_callback(void *argument)
{
	/** @brief Page names in the order their modes are enumerated. */
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

/**
 * @brief LVGL callback that rebuilds the UI a number of times.
 *
 * @details Rebuild the whole UI N times; comparing the heap before and after
 * turns a leak of any per-rebuild buffer into an obvious number. The final
 * show_page() leaves the visible page consistent with the mode.
 *
 * @param[in] argument Number of rebuilds, passed as an integer in the pointer.
 */
static void ui_memtest_callback(void *argument)
{
	uint32_t runs = (uint32_t)(uintptr_t)argument;

	for (uint32_t index = 0U; index < runs; index++) {
		cardsight_ui_refresh(NULL);
	}
	show_page(ui.mode);
	ui_command_result = 0;
	up(&ui_command_done);
}

/**
 * @brief LVGL callback that runs the UI memory guards.
 *
 * @details The fade lists and the LVGL draw buffer are checked while the LVGL
 * task owns the display, so the checks do not race with a redraw.
 *
 * @param[in] argument Unused.
 */
static void ui_memcheck_callback(void *argument)
{
	int failures = 0;

	(void)argument;
	failures += ui_list_fades_check();
	failures += lvgl_port_check_buffer() != 0 ? 1 : 0;
	ui_command_result = failures == 0 ? 0 : -EINVAL;
	up(&ui_command_done);
}

/**
 * @brief Entry point of the cardsight shell command.
 *
 * @details The subcommands cover board diagnostics that are easier to drive
 * from the console than from the UI:
 * - "usb-test" starts a read-only proxy and USB device, then stops them;
 * - "usb on|off|disk" starts the proxy-backed device, stops it, or exposes
 *   the whole card read-only for host-side tests;
 * - "ui-test" runs the object-tree selftest on the LVGL task;
 * - "page <name>" switches to a page, "radius [px]" opens calibration;
 * - "memtest [n]" rebuilds the UI n times and compares the heaps;
 * - "memcheck" runs the UI memory guards.
 * Each LVGL callback is posted and waited for with a timeout, so a hung UI is
 * reported as -ETIMEDOUT instead of blocking the shell.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or a negative errno naming the failure.
 */
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
	if (argc >= 2 && strcmp(argv[1], "memtest") == 0) {
		uint32_t runs = argc == 3 ? (uint32_t)atoi(argv[2]) : 5U;
		struct mallinfo before;
		struct mallinfo after;
		HeapStats_t kernel_before;
		HeapStats_t kernel_after;

		if (runs == 0U) {
			runs = 1U;
		} else if (runs > 50U) {
			runs = 50U;
		}
		before = mallinfo();
		vPortGetHeapStats(&kernel_before);
		if (lvgl_port_post(ui_memtest_callback, (void *)(uintptr_t)runs) != 0 ||
			down_timeout(&ui_command_done, 30000) != 0) {
			return -ETIMEDOUT;
		}
		after = mallinfo();
		vPortGetHeapStats(&kernel_after);
		printf("memtest %u rebuilds: c heap %d -> %d (%+d), kernel free %u -> %u (%+d)\n",
			runs, (int)before.uordblks, (int)after.uordblks,
			(int)(after.uordblks - before.uordblks),
			(uint32_t)kernel_before.xAvailableHeapSpaceInBytes,
			(uint32_t)kernel_after.xAvailableHeapSpaceInBytes,
			(int)(kernel_after.xAvailableHeapSpaceInBytes -
				kernel_before.xAvailableHeapSpaceInBytes));
		printf("memtest: %s\n", after.uordblks > before.uordblks + 16384 ?
			"LEAK" : "clean");
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "memcheck") == 0) {
		if (lvgl_port_post(ui_memcheck_callback, NULL) != 0 ||
			down_timeout(&ui_command_done, 5000) != 0) {
			return -ETIMEDOUT;
		}
		printf("CardSight memory guards: %s\n", ui_command_result == 0 ? "PASS" : "FAIL");
		return ui_command_result;
	}
	if (argc >= 2 && strcmp(argv[1], "radius") == 0) {
		ui_requested_radius = argc == 3 ? atoi(argv[2]) : 0;
		if (lvgl_port_post(ui_radius_callback, NULL) != 0 || down_timeout(&ui_command_done, 5000) != 0) {
			return -ETIMEDOUT;
		}
		return ui_command_result;
	}
	printf("cardsight page <direct|proxy|disk|info|layout> | radius [px] | ui-test | memtest [n] | memcheck | usb-test | usb <on|off|disk>\n");
	return -EINVAL;
}

/** @brief Register @c cardsight as a shell command with its usage text. */
SHELL_CMD(cardsight, "cardsight page <name> | radius [px] | ui-test | memtest [n] | memcheck | usb-test | usb <on|off|disk>: board diagnostics", command_cardsight);
#endif

/**
 * @brief Create the semaphore the shell callbacks release.
 *
 * @details It starts at zero with a limit of one, so a command posts a
 * callback and waits for exactly one completion; a leftover release cannot
 * satisfy the next command.
 */
void cardsight_diagnostics_init(void)
{
#ifdef CONFIG_SHELL
	sema_init_limit(&ui_command_done, 0U, 1U);
#endif
}
