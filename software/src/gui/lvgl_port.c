/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LVGL on this board: one display, the touch panel as the pointer, and a request queue so other
 * tasks can run a function in the LVGL task. The LVGL task renders into a partial draw buffer
 * and the flush writes the area straight to the panel.
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

struct lvgl_request {
	lvgl_port_callback_t callback;
	void *argument;
};

static struct {
	struct message_queue requests;
	lv_display_t *display;
	lv_indev_t *input;
	void *draw_buffer;
	uint16_t width;
	uint16_t height;
	uint64_t flush_pixels;
	uint64_t flush_time_us;
	uint32_t flush_count;
	uint32_t flush_max_us;
	volatile bool ready;
	volatile int error;
} port;

static uint32_t lvgl_tick(void)
{
	return (uint32_t)(ktime_get_us() / 1000U);
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
	uint16_t height = (uint16_t)(area->y2 - area->y1 + 1);
	uint64_t started_us = ktime_get_us();
	uint32_t elapsed_us;
	int result;

	/* LVGL renders RGB888 (B, G, R); the panel takes 18 bits per pixel, R, G, B. */
	{
		uint8_t *pixel = pixels;

		for (size_t index = 0U; index < (size_t)width * height; index++) {
			uint8_t blue = pixel[0];
			uint8_t green = pixel[1];
			uint8_t red = pixel[2];

			pixel[0] = (uint8_t)(red & 0xfcU);
			pixel[1] = (uint8_t)(green & 0xfcU);
			pixel[2] = (uint8_t)(blue & 0xfcU);
			pixel += 3U;
		}
	}
	result = sunxi_display_write((uint16_t)area->x1, (uint16_t)area->y1, width, height, width, pixels);
	elapsed_us = (uint32_t)(ktime_get_us() - started_us);
	port.flush_count++;
	port.flush_pixels += (uint64_t)width * height;
	port.flush_time_us += elapsed_us;
	if (elapsed_us > port.flush_max_us) {
		port.flush_max_us = elapsed_us;
	}
	if (result != 0) {
		port.error = result;
	}
	lv_display_flush_ready(display);
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
	port.draw_buffer = aligned_alloc(64U, (buffer_size + 63U) & ~(size_t)63U);
	if (port.draw_buffer == NULL) {
		return -ENOMEM;
	}
	lv_init();
	lv_tick_set_cb(lvgl_tick);
	port.display = lv_display_create(info.width, info.height);
	if (port.display == NULL) {
		return -ENOMEM;
	}
	lv_display_set_color_format(port.display, LV_COLOR_FORMAT_RGB888);
	lv_display_set_buffers(port.display, port.draw_buffer, NULL, (uint32_t)buffer_size,
		LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(port.display, lvgl_flush);

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
	pr_info("LVGL 9.6: %ldx%ld RGB565%s\n", (long)lv_display_get_horizontal_resolution(port.display),
		(long)lv_display_get_vertical_resolution(port.display),
		port.input != NULL ? ", touchscreen" : "");
	for (;;) {
		uint32_t delay_ms;

		while (msg_queue_get(&port.requests, &request, 0) == 0) {
			request.callback(request.argument);
		}
		delay_ms = lv_timer_handler();
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
	printf("%u flushes, %llu pixels, %llu us, %u us max, error %d\n", (uint32_t)port.flush_count,
		(unsigned long long)port.flush_pixels, (unsigned long long)port.flush_time_us,
		(uint32_t)port.flush_max_us, port.error);
	return 0;
}
SHELL_CMD(lvgl, "lvgl [reset]: the display flush statistics", command_lvgl);
#endif
