/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LVGL on this board: one display, the touch panel as the pointer, and a request queue so other
 * tasks can run a function in the LVGL task. The LVGL task renders into a partial draw buffer
 * in the boot SRAM and the flush writes the area straight to the panel.
 */

#define LOG_MODULE "lvgl"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <drm/sunxi_display.h>
#include <gui/lvgl_port.h>
#include <hal/kernel.h>
#include <init.h>
#include <log.h>
#include <lvgl.h>
#include <shell.h>

#include "drivers/drivers.h"

#define DRAW_LINES	 40U
#define REQUEST_COUNT	 8U
#define TASK_STACK_WORDS 4096U
#define TASK_PRIORITY	 2U
#define TASK_CREATE_TRIES 100U
#define FLUSH_TIMEOUT_US 100000U
#define DRAW_GUARD	 0x4c564744U /* "LVGD" */

struct lvgl_request {
	lvgl_port_callback_t callback;
	void *argument;
};

static struct {
	struct message_queue requests;
	lv_display_t *display;
	lv_indev_t *input;
	void *draw_buffer;
	uint32_t *draw_guard;
	uint16_t width;
	uint16_t height;
	uint64_t flush_pixels;
	uint64_t flush_time_us;
	uint32_t flush_count;
	uint32_t flush_max_us;
	uint64_t flush_started_us;
	volatile bool flush_pending;
	volatile bool ready;
	volatile int error;
} port;

static uint32_t lvgl_tick(void)
{
	return (uint32_t)(ktime_get_us() / 1000U);
}

/* Called when the wire has taken the last byte: release the buffer and count the transfer. */
static void lvgl_flush_complete(int result)
{
	uint32_t elapsed_us = (uint32_t)(ktime_get_us() - port.flush_started_us);

	port.flush_pending = false;
	port.flush_time_us += elapsed_us;
	if (elapsed_us > port.flush_max_us) {
		port.flush_max_us = elapsed_us;
	}
	if (result != 0) {
		port.error = result;
	}
	lv_display_flush_ready(port.display);
}

/* One step of the wire: true once nothing is pending any more. A panel that does not finish
 * in time is given up on, so a stuck transfer cannot freeze the UI. */
static bool lvgl_flush_finish(void)
{
	int result;

	if (!port.flush_pending) {
		return true;
	}
	if (sunxi_display_write_done(&result)) {
		lvgl_flush_complete(result);
		return true;
	}
	if (ktime_get_us() - port.flush_started_us > FLUSH_TIMEOUT_US) {
		lvgl_flush_complete(-ETIMEDOUT);
		return true;
	}
	return false;
}

int lvgl_port_check_buffer(void)
{
	if (port.draw_guard == NULL) {
		return 0;
	}
	if (*port.draw_guard != DRAW_GUARD) {
		pr_err("draw buffer guard broken: %08x\n", (unsigned)*port.draw_guard);
		return -1;
	}
	return 0;
}

void lvgl_port_wait_idle(void)
{
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
}

/* LVGL calls this instead of spinning when it needs a draw buffer back. */
static void lvgl_flush_wait(lv_display_t *display)
{
	(void)display;
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
}

/* The pixels go out through the second hart: the window command is synchronous, the pixel
 * stream is not, and LVGL renders the next area while the first is still on the wire. */
static void lvgl_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
	uint16_t height = (uint16_t)(area->y2 - area->y1 + 1);
	int result;

	(void)display;
	/* the previous stream has to be over before the panel takes a new window command */
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
	sunxi_display_wait();
	result = sunxi_display_write_async((uint16_t)area->x1, (uint16_t)area->y1, width, height,
		width, pixels);
	port.flush_count++;
	port.flush_pixels += (uint64_t)width * height;
	if (result != 0) {
		port.error = result;
		lv_display_flush_ready(port.display);
		return;
	}
	port.flush_started_us = ktime_get_us();
	port.flush_pending = true;
}

static void lvgl_touch_read(lv_indev_t *input, lv_indev_data_t *data)
{
	uint16_t x_pos;
	uint16_t y_pos;
	bool pressed;

	(void)input;
	if (sunxi_touch_read(&x_pos, &y_pos, &pressed) != 0) {
		data->state = LV_INDEV_STATE_RELEASED;
		return;
	}
	/* the panel counts from the other side of the glass */
	data->point.x = (int32_t)(((uint32_t)(port.width - 1U - MIN(x_pos, port.width - 1U)) * port.width) /
				  SUNXI_TOUCH_WIDTH);
	data->point.y = (int32_t)(((uint32_t)MIN(y_pos, port.height - 1U) * port.height) / SUNXI_TOUCH_HEIGHT);
	data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static int lvgl_open_devices(void)
{
	struct sunxi_display_info info;
	uint16_t lines;
	size_t buffer_size;

	if (sunxi_display_get_info(&info) != 0 || info.bytes_per_pixel != 3U) {
		return -ENODEV;
	}
	port.width = info.width;
	port.height = info.height;
	lines = MIN(DRAW_LINES, info.height);
	buffer_size = (size_t)info.width * lines * 3U;
	/* LVGL writes every rendered pixel here; the SRAM takes those stores far faster than
	 * the PSRAM does (over 400 MB/s against about 11 MB/s). */
	if (buffer_size > SUNXI_SRAM_BUFFER_SIZE) {
		return -ENOMEM;
	}
	port.draw_buffer = (void *)SUNXI_SRAM_BUFFER_ADDRESS;
	lv_init();
	lv_tick_set_cb(lvgl_tick);
	port.display = lv_display_create(info.width, info.height);
	if (port.display == NULL) {
		return -ENOMEM;
	}
	lv_display_set_color_format(port.display, LV_COLOR_FORMAT_RGB888);
	lv_display_set_buffers(port.display, port.draw_buffer, NULL, (uint32_t)buffer_size,
		LV_DISPLAY_RENDER_MODE_PARTIAL);
	{
		/* A word right after the draw buffer catches an off-by-one area write. The SRAM
		 * after the buffer is unused until the panel mailbox. */
		uintptr_t guard = ((uintptr_t)port.draw_buffer + buffer_size + 3U) & ~(uintptr_t)3U;

		if (guard + sizeof(uint32_t) <= (uintptr_t)SUNXI_SRAM_BUFFER_ADDRESS +
			SUNXI_SRAM_BUFFER_SIZE) {
			port.draw_guard = (uint32_t *)guard;
			*port.draw_guard = DRAW_GUARD;
		}
	}
	lv_display_set_flush_cb(port.display, lvgl_flush);
	lv_display_set_flush_wait_cb(port.display, lvgl_flush_wait);

	if (sunxi_touch_ready()) {
		port.input = lv_indev_create();
		if (port.input == NULL) {
			return -ENOMEM;
		}
		lv_indev_set_type(port.input, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(port.input, lvgl_touch_read);
		lv_indev_set_display(port.input, port.display);
	}
	return 0;
}

static void lvgl_task(void *argument)
{
	struct lvgl_request request;
	int result = -ENODEV;

	(void)argument;
	/* the display and the touch controller come up from init calls; wait for them */
	for (uint32_t tries = 0U; tries < TASK_CREATE_TRIES; tries++) {
		result = lvgl_open_devices();
		if (result != -ENODEV) {
			break;
		}
		msleep(20U);
	}
	if (result != 0) {
		port.error = result;
		pr_err("cannot bring LVGL up: %d\n", result);
		vTaskDelete(NULL);
		return;
	}
	port.ready = true;
	pr_info("LVGL 9.6: %ldx%ld RGB888%s\n", (long)lv_display_get_horizontal_resolution(port.display),
		(long)lv_display_get_vertical_resolution(port.display),
		port.input != NULL ? ", touchscreen" : "");
	for (;;) {
		uint32_t delay_ms;

		while (msg_queue_get(&port.requests, &request, 0) == 0) {
			request.callback(request.argument);
		}
		if (port.flush_pending) {
			(void)lvgl_flush_finish();
		}
		delay_ms = lv_timer_handler();
		if (port.flush_pending && delay_ms > 1U) {
			delay_ms = 1U; /* keep the end of the wire in sight */
		}
		msleep((int)(delay_ms < 1U ? 1U : (delay_ms > 20U ? 20U : delay_ms)));
	}
}

int lvgl_port_post(lvgl_port_callback_t callback, void *argument)
{
	struct lvgl_request request = { .callback = callback, .argument = argument };

	if (callback == NULL) {
		return -EINVAL;
	}
	if (!port.ready) {
		return -EAGAIN;
	}
	return msg_queue_put(&port.requests, &request, 0);
}

bool lvgl_port_ready(void)
{
	return port.ready;
}

int lvgl_port_last_error(void)
{
	return port.error;
}

static int lvgl_port_init(void)
{
	if (!sunxi_display_ready()) {
		/* the panel driver runs at priority 30 of the same level */
		msleep(50U);
	}
	if (msg_queue_init(&port.requests, sizeof(struct lvgl_request), REQUEST_COUNT) != 0) {
		return -ENOMEM;
	}
	return xTaskCreate(lvgl_task, "lvgl", TASK_STACK_WORDS, NULL, TASK_PRIORITY, NULL) == pdPASS ?
		       0 :
		       -ENOMEM;
}
INITCALL(3, 20, lvgl_port_init);

#ifdef CONFIG_SHELL
static uint64_t bench_rate_kib(size_t bytes, uint32_t frames, uint64_t elapsed_us)
{
	return (uint64_t)bytes * frames * 1000000U / 1024U / (elapsed_us == 0U ? 1U : elapsed_us);
}

static void bench_wait_idle(void)
{
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
}

static uint64_t bench_redraw(uint32_t frames)
{
	uint64_t started = ktime_get_us();

	for (uint32_t frame = 0U; frame < frames; frame++) {
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
		bench_wait_idle();
	}
	return ktime_get_us() - started;
}

/* The parts of a frame, one stage at a time: memory, the wire, a plain image, the same image
 * blended, then the real UI with the wire time split out of the wall time. */
static void lvgl_bench_run(void *argument)
{
	uint32_t frames = 20U;
	size_t bytes = (size_t)port.width * port.height * 3U;
	uint8_t *buffer = pvPortMalloc(bytes);
	uint8_t *copy = pvPortMalloc(bytes);
	lv_image_dsc_t image = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RGB888,
			.w = port.width,
			.h = port.height,
			.stride = port.width * 3U,
		},
		.data_size = (uint32_t)bytes,
		.data = buffer,
	};
	lv_obj_t *original;
	lv_obj_t *scratch;
	lv_obj_t *picture;
	uint64_t started;
	uint64_t elapsed;

	(void)argument;
	if (buffer == NULL || copy == NULL) {
		printf("bench: out of memory\n");
		goto out;
	}
	for (size_t index = 0U; index < bytes; index++) {
		buffer[index] = (uint8_t)(index * 31U);
	}
	bench_wait_idle();
	started = ktime_get_us();
	for (uint32_t frame = 0U; frame < frames; frame++) {
		memcpy(copy, buffer, bytes);
	}
	elapsed = ktime_get_us() - started;
	printf("bench: copy %u KiB x%u: %llu KiB/s\n", (uint32_t)(bytes / 1024U), (uint32_t)frames,
		(unsigned long long)bench_rate_kib(bytes * 2U, frames, elapsed));

	started = ktime_get_us();
	for (uint32_t frame = 0U; frame < frames; frame++) {
		(void)sunxi_display_write(0U, 0U, port.width, port.height, port.width, buffer);
	}
	elapsed = ktime_get_us() - started;
	printf("bench: wire %u KiB x%u: %llu us (%llu KiB/s)\n", (uint32_t)(bytes / 1024U),
		(uint32_t)frames, (unsigned long long)elapsed,
		(unsigned long long)bench_rate_kib(bytes, frames, elapsed));

	original = lv_screen_active();
	scratch = lv_obj_create(NULL);
	picture = lv_image_create(scratch);
	lv_image_set_src(picture, &image);
	lv_obj_set_pos(picture, 0, 0);
	lv_screen_load(scratch);
	elapsed = bench_redraw(10U);
	printf("bench: image x10: %llu us each\n", (unsigned long long)(elapsed / 10U));
	lv_obj_set_style_image_opa(picture, 160, 0);
	elapsed = bench_redraw(10U);
	printf("bench: image opa 160 x10: %llu us each\n", (unsigned long long)(elapsed / 10U));
	lv_screen_load(original);
	lv_obj_delete(scratch);

	port.flush_count = 0U;
	port.flush_pixels = 0U;
	port.flush_time_us = 0U;
	port.flush_max_us = 0U;
	elapsed = bench_redraw((uint32_t)frames);
	printf("bench: ui redraw x%u: %llu us each (%u flushes, %llu px, %llu us of wire each)\n",
		(uint32_t)frames, (unsigned long long)(elapsed / frames), (uint32_t)port.flush_count,
		(unsigned long long)(port.flush_pixels / frames),
		(unsigned long long)(port.flush_time_us / frames));
out:
	if (copy != NULL) {
		vPortFree(copy);
	}
	if (buffer != NULL) {
		vPortFree(buffer);
	}
}

static int command_lvgl(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "reset") == 0) {
		port.flush_pixels = 0U;
		port.flush_time_us = 0U;
		port.flush_count = 0U;
		port.flush_max_us = 0U;
		port.error = 0;
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "bench") == 0) {
		int result = lvgl_port_post(lvgl_bench_run, NULL);

		if (result != 0) {
			printf("LVGL is not up\n");
		}
		return result;
	}
	printf("%u flushes, %llu pixels, %llu us, %u us max, error %d\n", (uint32_t)port.flush_count,
		(unsigned long long)port.flush_pixels, (unsigned long long)port.flush_time_us,
		(uint32_t)port.flush_max_us, port.error);
	return 0;
}
SHELL_CMD(lvgl, "lvgl [reset|bench]: the display flush statistics, or a wire and redraw benchmark", command_lvgl);
#endif
