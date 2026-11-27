// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_web.c
 * @brief Browser harness around the real CardSight LVGL UI.
 *
 * The build compiles LVGL, the five pages, the widgets and the glass material
 * for WebAssembly. This file supplies the pieces the board provides in
 * firmware: a display whose flush lands in a memory framebuffer, a pointer
 * input device, a fixed 32 GB card fixture, and a request emulator that answers
 * the worker messages the pages send. The browser copies the framebuffer to a
 * canvas and drives the loop with cardsight_web_tick().
 */

#include "ui/ui_private.h"

#include <bits.h>
#include <emscripten.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Simulated panel width in pixels, matching the ST7789 panel. */
#define WEB_WIDTH 240
/** @brief Simulated panel height in pixels, matching the ST7789 panel. */
#define WEB_HEIGHT 284

/** @brief Put the fixture back and return to the Direct page. */
#define WEB_ACTION_RESET	     0
/** @brief Insert the fixture card. */
#define WEB_ACTION_INSERT	     1
/** @brief Remove the card and drop what was serving it. */
#define WEB_ACTION_EJECT	     2
/** @brief Start the USB link (direct or proxy, following the visible page). */
#define WEB_ACTION_CONNECT	     3
/** @brief Stop the USB link and the proxy view. */
#define WEB_ACTION_STOP		     4
/** @brief Show the card-identification spinner, then insert the card. */
#define WEB_ACTION_DETECT	     5
/** @brief Show the error sheet for a failed card read. */
#define WEB_ACTION_ERROR	     6
/** @brief Open the format confirmation sheet. */
#define WEB_ACTION_FORMAT_DIALOG     7
/** @brief Open the whole-card partition layout editor. */
#define WEB_ACTION_LAYOUT_EDITOR     8
/** @brief Open the file browser on the exFAT partition's root. */
#define WEB_ACTION_FILES_DETAIL	     9

/** @brief The model snapshot the UI reads; only the emulator writes it. */
static struct cardsight_state model;
/** @brief RGB565 render target, large enough for one full frame. */
static uint16_t framebuffer[WEB_WIDTH * WEB_HEIGHT] __attribute__((aligned(64)));
/** @brief Simulated milliseconds since boot, advanced by cardsight_web_tick(). */
static uint32_t tick_ms;
/** @brief True after a flush, so the browser only copies changed frames. */
static bool frame_dirty;
/** @brief Pointer state fed to LVGL by the input-device read callback. */
static lv_indev_data_t pointer;
/** @brief True while the identification demo is counting down. */
static bool detect_pending;
/** @brief Tick at which the identification demo inserts the card. */
static uint32_t detect_deadline;
/** @brief The LVGL display of the simulated panel. */
static lv_display_t *display;

/**
 * @brief LVGL tick callback backed by the simulated clock.
 *
 * @return Current simulated time in milliseconds.
 */
static uint32_t web_tick(void)
{
	return tick_ms;
}

/**
 * @brief Browser-side logger; forwards to the console through stderr.
 *
 * @param[in] level  Log level; ignored.
 * @param[in] module Module name; ignored.
 * @param[in] format printf-style message format.
 * @param[in] ...    Arguments for @p format.
 */
void log_print(int level, const char *module, const char *format, ...)
{
	va_list arguments;

	(void)level;
	(void)module;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

/**
 * @brief Report the simulated panel geometry to the UI.
 *
 * @param[out] info Display information to fill.
 * @return 0.
 */
int sunxi_display_get_info(struct sunxi_display_info *info)
{
	memset(info, 0, sizeof(*info));
	info->width = WEB_WIDTH;
	info->height = WEB_HEIGHT;
	info->fb_width = WEB_WIDTH;
	info->fb_height = WEB_HEIGHT;
	info->stride = WEB_WIDTH * 2U;
	info->bytes_per_pixel = 2U;
	info->pixels_swapped = true;
	return 0;
}

/**
 * @brief Flush callback; the full frame is already in @c framebuffer.
 *
 * @param[in] display Display being flushed.
 * @param[in] area    Changed area; ignored in full-render mode.
 * @param[in] pixels  Source pixels; ignored.
 */
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	frame_dirty = true;
	lv_display_flush_ready(display);
}

/**
 * @brief Input-device read callback: report the browser's pointer state.
 *
 * @param[in]  indev Input device; not inspected.
 * @param[out] data  State LVGL should process this cycle.
 */
static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	*data = pointer;
}

/**
 * @brief Copy one fixture model into the state the UI reads.
 *
 * @details Sets a 32 GB card with a valid identity and three 10 GB partitions
 *          formatted as FAT32, exFAT, and ext4 (the last one read-only).
 */
static void fixture(void)
{
	/** @brief One filesystem per fixture partition. */
	static const char *const types[] = { "FAT32", "exFAT", "ext4" };
	/** @brief Partition labels shown in the fixture. */
	static const char *const labels[] = { "BOOT", "PHOTOS", "LINUX" };
	/** @brief The exFAT root listing, including a long UTF-8 name. */
	static const struct {
		const char *name;
		uint64_t size;
		bool dir;
	} files[] = {
		{ "A-very-long-file-name-used-to-check-ellipsis.txt", 88123U, false },
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

/**
 * @brief One directory the emulated worker can list.
 */
struct web_dir {
	/** @brief Partition number the directory belongs to. */
	uint32_t partition;
	/** @brief Absolute path inside the partition. */
	const char *path;
	/** @brief Entries of the directory. */
	const struct cardsight_file_entry *entries;
	/** @brief Number of entries. */
	size_t count;
};

/** @brief Root of the FAT32 boot partition. */
static const struct cardsight_file_entry boot_root[] = {
	{ "System Volume Information", 0, true },
	{ "DCIM", 0, true },
	{ "boot.log", 4096, false },
	{ "config.txt", 512, false },
};
/** @brief A photo directory on the boot partition. */
static const struct cardsight_file_entry boot_dcim[] = {
	{ "100CANON", 0, true },
	{ "IMG_0001.JPG", 3800000, false },
	{ "IMG_0002.JPG", 4100000, false },
};
/** @brief Root of the exFAT photo partition. */
static const struct cardsight_file_entry photo_root[] = {
	{ "A-very-long-file-name-used-to-check-ellipsis.txt", 88123, false },
	{ "DCIM", 0, true },
	{ "Music", 0, true },
	{ "System Volume Information", 0, true },
	{ "hello.txt", 11, false },
	{ "IMG_20261106_123456.jpg", 4200000, false },
	{ "README.md", 1234, false },
};
/** @brief The photo folder on the exFAT partition. */
static const struct cardsight_file_entry photo_dcim[] = {
	{ "100CANON", 0, true },
	{ "IMG_20261106_123456.jpg", 4200000, false },
	{ "IMG_20261106_123457.jpg", 3900000, false },
	{ "IMG_20261106_123458.jpg", 4400000, false },
	{ "VID_20261106_130001.mp4", 88000000, false },
};
/** @brief The photo subfolder. */
static const struct cardsight_file_entry photo_canon[] = {
	{ "IMG_0001.JPG", 3700000, false },
	{ "IMG_0002.JPG", 3600000, false },
	{ "IMG_0003.JPG", 3800000, false },
};
/** @brief The music folder on the exFAT partition. */
static const struct cardsight_file_entry photo_music[] = {
	{ "01 - opening.flac", 28000000, false },
	{ "02 - theme.flac", 31000000, false },
	{ "cover.jpg", 240000, false },
};
/** @brief Root of the ext4 partition. */
static const struct cardsight_file_entry linux_root[] = {
	{ "etc", 0, true },
	{ "home", 0, true },
	{ "lost+found", 0, true },
	{ "var", 0, true },
	{ "vmlinuz-6.12", 12000000, false },
};
/** @brief /etc of the ext4 partition. */
static const struct cardsight_file_entry linux_etc[] = {
	{ "fstab", 720, false },
	{ "hostname", 12, false },
	{ "passwd", 1800, false },
};
/** @brief /home of the ext4 partition. */
static const struct cardsight_file_entry linux_home[] = {
	{ "cardsight", 0, true },
	{ "README", 220, false },
};
/** @brief Every directory the emulated worker knows. */
static const struct web_dir directories[] = {
	{ 1U, "/", boot_root, ARRAY_SIZE(boot_root) },
	{ 1U, "/DCIM", boot_dcim, ARRAY_SIZE(boot_dcim) },
	{ 2U, "/", photo_root, ARRAY_SIZE(photo_root) },
	{ 2U, "/DCIM", photo_dcim, ARRAY_SIZE(photo_dcim) },
	{ 2U, "/DCIM/100CANON", photo_canon, ARRAY_SIZE(photo_canon) },
	{ 2U, "/Music", photo_music, ARRAY_SIZE(photo_music) },
	{ 2U, "/System Volume Information", NULL, 0 },
	{ 3U, "/", linux_root, ARRAY_SIZE(linux_root) },
	{ 3U, "/etc", linux_etc, ARRAY_SIZE(linux_etc) },
	{ 3U, "/home", linux_home, ARRAY_SIZE(linux_home) },
};

/**
 * @brief Fill the model's file listing from the directory table.
 *
 * @details An unknown path yields a valid listing with no entries, which is
 *          what the page shows for an empty directory.
 *
 * @param[in] partition One-based partition number.
 * @param[in] path      Directory path inside the partition.
 */
static void list_directory(uint32_t partition, const char *path)
{
	struct cardsight_file_list *list = &model.files;

	memset(list, 0, sizeof(*list));
	list->partition = partition;
	snprintf(list->path, sizeof(list->path), "%s", path != NULL ? path : "/");
	list->valid = true;
	list->error = 0;
	for (uint32_t index = 0U; index < ARRAY_SIZE(directories); index++) {
		const struct web_dir *dir = &directories[index];

		if (dir->partition != partition || strcmp(dir->path, list->path) != 0) {
			continue;
		}
		list->total = dir->count;
		list->count = MIN(dir->count, (size_t)APP_FILE_ENTRIES);
		for (size_t entry = 0U; entry < list->count; entry++) {
			list->entries[entry] = dir->entries[entry];
		}
		list->truncated = dir->count > APP_FILE_ENTRIES;
		return;
	}
}

/**
 * @brief Emulated worker: apply the requests the pages send.
 *
 * @details There is no card and no USB stack in the browser. The emulator
 *          keeps the model coherent so every page reacts the way it does on the
 *          board: a list request fills the browser, a start request raises the
 *          link, and a stop request takes the proxy down. A deferred rebuild is
 *          scheduled because the pages do not all rebuild themselves.
 *
 * @param[in] request Request the page submitted.
 */
void send_request(const struct cardsight_request *request)
{
	switch (request->type) {
	case CARDSIGHT_REQUEST_LIST:
		list_directory(request->partition, request->path);
		break;
	case CARDSIGHT_REQUEST_START_DIRECT:
	case CARDSIGHT_REQUEST_START_PROXY:
		if (!model.card_present) {
			model.error = -ENODEV;
			model.request = request->type;
			snprintf(model.operation, sizeof(model.operation), "%s", "Connect failed");
			snprintf(model.card_error, sizeof(model.card_error), "%s", "No card detected");
			break;
		}
		model.running = true;
		model.configured = true;
		model.proxy_active = request->type == CARDSIGHT_REQUEST_START_PROXY;
		model.busy = false;
		model.progress = -1;
		model.error = 0;
		snprintf(model.operation, sizeof(model.operation), "%s", "USB connected");
		break;
	case CARDSIGHT_REQUEST_STOP:
		model.running = false;
		model.configured = false;
		model.proxy_active = false;
		model.busy = false;
		model.progress = -1;
		model.error = 0;
		model.read_rate = 0U;
		model.write_rate = 0U;
		snprintf(model.operation, sizeof(model.operation), "%s", "Stopped safely");
		break;
	case CARDSIGHT_REQUEST_FORMAT:
		model.busy = false;
		model.progress = -1;
		model.error = 0;
		snprintf(model.operation, sizeof(model.operation), "%s", "Format complete");
		break;
	case CARDSIGHT_REQUEST_LAYOUT:
		model.busy = false;
		model.progress = -1;
		model.error = 0;
		snprintf(model.operation, sizeof(model.operation), "%s", "Partition table rebuilt");
		break;
	case CARDSIGHT_REQUEST_SCAN:
	default:
		break;
	}
	ui_schedule_rebuild();
}

/**
 * @brief Copy the emulated model to the UI.
 *
 * @param[out] state Snapshot destination.
 */
void cardsight_snapshot(struct cardsight_state *state)
{
	*state = model;
}

/**
 * @brief Put the fixture card back in the slot.
 *
 * @param[in] present True to insert the card, false to remove it.
 */
static void set_card_present(bool present)
{
	model.card_present = present;
	model.card_writable = present;
	model.card_info_valid = present;
	model.partition_count = present ? 3U : 0U;
	if (!present) {
		model.running = false;
		model.configured = false;
		model.proxy_active = false;
		model.read_rate = 0U;
		model.write_rate = 0U;
		model.files.valid = false;
		ui.files_detail = false;
		ui.manage_detail = false;
		ui.format_dialog_open = false;
		ui.layout_editor = false;
	}
	model.card_detecting = false;
}

/**
 * @brief Reset every view flag the fixture setup depends on.
 */
static void reset_ui_state(void)
{
	ui.selected = 0U;
	ui.selection_initialized = false;
	ui.layout_count = 0U;
	ui.layout_sectors = 0U;
	ui.manage_detail = false;
	ui.files_detail = false;
	ui.files_partition = 2U;
	ui.format_dialog_open = false;
	ui.layout_editor = false;
	ui.dismissed_error = 0;
	ui.mode = CARDSIGHT_DIRECT;
	snprintf(ui.files_path, sizeof(ui.files_path), "/");
}

/**
 * @brief Initialise LVGL, the panel and the fixture.
 *
 * @details Idempotent, so a page can call it on every mount.
 */
EMSCRIPTEN_KEEPALIVE void cardsight_web_init(void)
{
	lv_indev_t *indev;

	if (display != NULL) {
		return;
	}
	lv_init();
	lv_tick_set_cb(web_tick);
	display = lv_display_create(WEB_WIDTH, WEB_HEIGHT);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer),
		LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(display, flush);

	memset(&pointer, 0, sizeof(pointer));
	pointer.state = LV_INDEV_STATE_RELEASED;
	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, pointer_read);
	lv_indev_set_display(indev, display);

	/* start on the fixture card with the Direct page visible */
	memset(&model, 0, sizeof(model));
	model.progress = -1;
	reset_ui_state();
	fixture();
	cardsight_ui_refresh(NULL);
	show_page(CARDSIGHT_DIRECT);
	frame_dirty = true;
}

/**
 * @brief Advance the simulated clock and let LVGL run one cycle.
 *
 * @details Also animates the live transfer rates while the emulated link is up,
 *          and completes the "identifying card" scene after a short delay.
 *
 * @param[in] elapsed_ms Milliseconds since the previous call.
 */
EMSCRIPTEN_KEEPALIVE void cardsight_web_tick(uint32_t elapsed_ms)
{
	static uint32_t runtime_accumulator;

	tick_ms += elapsed_ms;
	if (detect_pending && (int32_t)(tick_ms - detect_deadline) >= 0) {
		detect_pending = false;
		set_card_present(true);
		cardsight_ui_refresh(NULL);
	}
	if (model.running) {
		uint32_t wobble = (tick_ms / 200U) % 7U;
		uint32_t read_rate = (62U * 1024U + 1024U - wobble * 96U) * 1024U;
		uint32_t write_rate = (9U * 1024U + wobble * 128U) * 1024U;

		model.read_rate = read_rate;
		model.write_rate = model.configured ? write_rate : 0U;
		model.bytes_read += read_rate / 60U;
		if (model.configured) {
			model.bytes_written += write_rate / 60U;
		}
	} else {
		model.read_rate = 0U;
		model.write_rate = 0U;
	}
	lv_timer_handler();
	runtime_accumulator += elapsed_ms;
	if (runtime_accumulator >= 100U) {
		runtime_accumulator = 0U;
		cardsight_ui_runtime(NULL);
	}
}

/** @brief Return the simulated panel width. */
EMSCRIPTEN_KEEPALIVE int cardsight_web_width(void)
{
	return WEB_WIDTH;
}

/** @brief Return the simulated panel height. */
EMSCRIPTEN_KEEPALIVE int cardsight_web_height(void)
{
	return WEB_HEIGHT;
}

/** @brief Return the address of the RGB565 framebuffer in wasm memory. */
EMSCRIPTEN_KEEPALIVE uint8_t *cardsight_web_framebuffer(void)
{
	return (uint8_t *)framebuffer;
}

/**
 * @brief Tell the browser whether a new frame was rendered.
 *
 * @return 1 when the frame changed since the last call, 0 otherwise.
 */
EMSCRIPTEN_KEEPALIVE int cardsight_web_dirty(void)
{
	int dirty = frame_dirty ? 1 : 0;

	frame_dirty = false;
	return dirty;
}

/**
 * @brief Feed a pointer event to LVGL.
 *
 * @param[in] x    Panel x coordinate.
 * @param[in] y    Panel y coordinate.
 * @param[in] down Non-zero while the finger is down.
 */
EMSCRIPTEN_KEEPALIVE void cardsight_web_pointer(int x, int y, int down)
{
	pointer.point.x = (int32_t)CLAMP(x, 0, WEB_WIDTH - 1);
	pointer.point.y = (int32_t)CLAMP(y, 0, WEB_HEIGHT - 1);
	pointer.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/**
 * @brief Return the visible tab index.
 *
 * @return 0 (direct) through 4 (info).
 */
EMSCRIPTEN_KEEPALIVE int cardsight_web_mode(void)
{
	return (int)ui.mode;
}

/**
 * @brief Switch the visible tab.
 *
 * @param[in] page Page index, 0 (direct) through 4 (info).
 */
EMSCRIPTEN_KEEPALIVE void cardsight_web_page(int page)
{
	if (display == NULL || page < 0 || page >= (int)APP_TAB_COUNT) {
		return;
	}
	ui.layout_editor = false;
	ui.manage_detail = false;
	show_page((enum cardsight_mode)page);
}

/**
 * @brief Run one scripted demo action from the docs page.
 *
 * @details The actions mirror what a finger and a card can do on the board;
 *          they keep the emulator state coherent across the five pages.
 *
 * @param[in] action One of the WEB_ACTION_* values.
 */
EMSCRIPTEN_KEEPALIVE void cardsight_web_action(int action)
{
	switch (action) {
	case WEB_ACTION_RESET:
		detect_pending = false;
		reset_ui_state();
		fixture();
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_DIRECT);
		break;
	case WEB_ACTION_INSERT:
		set_card_present(true);
		reset_ui_state();
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_DIRECT);
		break;
	case WEB_ACTION_EJECT:
		detect_pending = false;
		set_card_present(false);
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_DIRECT);
		break;
	case WEB_ACTION_CONNECT:
	{
		struct cardsight_request request = { .type = ui.mode == CARDSIGHT_PROXY ?
			CARDSIGHT_REQUEST_START_PROXY : CARDSIGHT_REQUEST_START_DIRECT };

		request.selected = ui.selected;
		send_request(&request);
		cardsight_ui_refresh(NULL);
		show_page(ui.mode);
		break;
	}
	case WEB_ACTION_STOP:
	{
		struct cardsight_request request = { .type = CARDSIGHT_REQUEST_STOP };

		send_request(&request);
		cardsight_ui_refresh(NULL);
		show_page(ui.mode);
		break;
	}
	case WEB_ACTION_DETECT:
		model.card_detecting = true;
		model.card_present = false;
		model.partition_count = 0U;
		model.running = false;
		model.configured = false;
		model.proxy_active = false;
		detect_pending = true;
		detect_deadline = tick_ms + 2400U;
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_DIRECT);
		break;
	case WEB_ACTION_ERROR:
		model.error = -EIO;
		model.request = CARDSIGHT_REQUEST_SCAN;
		model.card_detecting = false;
		snprintf(model.operation, sizeof(model.operation), "%s", "Card read/write error");
		snprintf(model.card_error, sizeof(model.card_error), "%s", "CMD5 Read Timeout");
		ui.dismissed_error = 0;
		cardsight_ui_refresh(NULL);
		break;
	case WEB_ACTION_FORMAT_DIALOG:
		/* show_page() clears detail/edit flags, so switch first and enter the view after. */
		show_page(CARDSIGHT_MANAGE);
		/* formatting is refused while a link is up; the demo opens it from the idle state */
		model.running = false;
		model.configured = false;
		model.proxy_active = false;
		ui.manage_detail = true;
		ui.manage_partition = 1U;
		ui.format_content = BLK_PARTITION_EXFAT;
		ui.format_dialog_open = true;
		cardsight_ui_refresh(NULL);
		break;
	case WEB_ACTION_LAYOUT_EDITOR:
		show_page(CARDSIGHT_MANAGE);
		ui.layout_editor = true;
		ui.layout_table = BLK_PARTITION_TABLE_MBR;
		ui.layout_content[0] = BLK_PARTITION_EXFAT;
		cardsight_ui_refresh(NULL);
		break;
	case WEB_ACTION_FILES_DETAIL:
		ui.mode = CARDSIGHT_FILES;
		ui.files_partition = 2U;
		snprintf(ui.files_path, sizeof(ui.files_path), "/");
		ui.files_detail = true;
		list_directory(2U, "/");
		cardsight_ui_refresh(NULL);
		show_page(CARDSIGHT_FILES);
		break;
	default:
		break;
	}
}
