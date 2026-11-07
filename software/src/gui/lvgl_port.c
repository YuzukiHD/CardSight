/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file lvgl_port.c
 * @brief LVGL port: one display, touch input, and a request queue for other tasks.
 *
 * The touch panel is the pointer, and a request queue lets other tasks run a function in the
 * LVGL task. The LVGL task renders into a partial draw buffer in the boot SRAM and the flush
 * writes the area straight to the panel.
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

/** @brief Rendered lines per draw buffer. */
#define DRAW_LINES	 40U
/** @brief Slots in the queue of functions posted to the LVGL task. */
#define REQUEST_COUNT	 8U
/** @brief Stack size of the LVGL task in words. */
#define TASK_STACK_WORDS 4096U
/** @brief Priority of the LVGL task. */
#define TASK_PRIORITY	 2U
/** @brief Attempts made to open the display before the task gives up, 20 ms apart. */
#define TASK_CREATE_TRIES 100U
/** @brief Time after which a flush to the panel is abandoned. */
#define FLUSH_TIMEOUT_US 100000U
/** @brief Guard word written after the draw buffer to catch an overrun, "LVGD". */
#define DRAW_GUARD	 0x4c564744U /* "LVGD" */

/**
 * @brief One function queued for the LVGL task.
 */
struct lvgl_request {
	lvgl_port_callback_t callback;	/**< Function to run in the LVGL task. */
	void *argument;			/**< Opaque argument passed to the callback. */
};

/**
 * @brief State shared by the LVGL task, the flush callbacks and the shell.
 *
 * @c ready is set only after the display and the input device are up, and @c error keeps the
 * first failure. The flush counters are read by the shell; they are written by the LVGL task
 * and the flush completion callback on the same core.
 */
static struct {
	struct message_queue requests;	/**< Functions posted to the LVGL task. */
	lv_display_t *display;		/**< LVGL display object. */
	lv_indev_t *input;		/**< Touch pointer device, or NULL when absent. */
	void *draw_buffer;		/**< Partial draw buffer in the boot SRAM. */
	uint32_t *draw_guard;		/**< Guard word after the buffer, or NULL. */
	uint16_t width;			/**< Display width in pixels. */
	uint16_t height;			/**< Display height in pixels. */
	uint64_t flush_pixels;		/**< Pixels sent to the panel, for the benchmark. */
	uint64_t flush_time_us;		/**< Accumulated transfer time in microseconds. */
	uint32_t flush_count;		/**< Number of flushes started. */
	uint32_t flush_max_us;		/**< Longest single transfer in microseconds. */
	uint64_t flush_started_us;	/**< Start of the pending transfer, in microseconds. */
	volatile bool flush_pending;	/**< A pixel stream is on the wire. */
	volatile bool ready;		/**< The display is open and the task loops. */
	volatile int error;		/**< First error recorded by the port. */
} port;

/**
 * @brief LVGL tick callback.
 *
 * @return Milliseconds since boot.
 */
static uint32_t lvgl_tick(void)
{
	return (uint32_t)(ktime_get_us() / 1000U);
}

/**
 * @brief Complete a flush once the panel has taken the last byte.
 *
 * @param[in] result 0, or the negative errno the transfer ended with.
 *
 * Called when the wire has taken the last byte: releases the draw buffer for LVGL, adds the
 * transfer time to the statistics, keeps the slowest one, records an error, and marks the flush
 * ready so LVGL can render again.
 */
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

/**
 * @brief Advance a pending flush by one step.
 *
 * @return True when nothing is pending any more.
 *
 * One step of the wire: when the panel reports the transfer done, the flush is completed. A
 * panel that does not finish within FLUSH_TIMEOUT_US is given up on with -ETIMEDOUT, so a stuck
 * transfer cannot freeze the UI.
 */
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

/**
 * @brief Check the guard word after the draw buffer.
 *
 * @return 0 when the guard is intact or absent, -1 when it was overwritten.
 *
 * An overwritten guard means LVGL rendered past the end of the buffer; the value is logged so
 * the area that caused it can be found.
 */
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

/**
 * @brief Wait until no pixel transfer is pending.
 *
 * Polls once per millisecond through lvgl_flush_finish(), which also releases the buffer and
 * times the transfer when the panel reports completion.
 */
void lvgl_port_wait_idle(void)
{
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
}

/**
 * @brief Wait for the draw buffer to come back from the panel.
 *
 * @param[in] display Display asking; unused.
 *
 * LVGL calls this instead of spinning when it needs a draw buffer back.
 */
static void lvgl_flush_wait(lv_display_t *display)
{
	(void)display;
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
}

/**
 * @brief Send one rendered area to the panel.
 *
 * @param[in] display Display being flushed; unused.
 * @param[in] area    Area that was rendered.
 * @param[in] pixels  Pixel data, contiguous rows of the area.
 *
 * The pixels go out through the second hart: the window command is synchronous, the pixel
 * stream is not, and LVGL renders the next area while the first is still on the wire. The
 * previous stream must be finished before a new window command is issued, so this waits for it
 * first. The transfer is left pending; lvgl_flush_finish() completes it later. A failed start
 * marks the display ready again immediately so LVGL does not stall.
 */
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

/**
 * @brief Read the touch panel for LVGL.
 *
 * @param[in]  input Input device; unused.
 * @param[out] data  Pointer state filled from the panel.
 *
 * A read error is reported as released, so a missing controller cannot leave a press stuck.
 * The x axis is mirrored to match the panel orientation, and both axes are scaled from the
 * controller's coordinate range to the display.
 */
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

/**
 * @brief Open the display and the optional touch panel and set LVGL up.
 *
 * @return 0 on success, -ENODEV for a missing or non-RGB888 display, or -ENOMEM.
 *
 * The draw buffer lives in the boot SRAM: LVGL writes every rendered pixel there, and the SRAM
 * takes those stores far faster than the PSRAM does. A partial buffer of DRAW_LINES lines is
 * used; the flush callback streams each finished area to the panel. A guard word is placed
 * after the buffer when it still fits in the SRAM window. The touch panel becomes the pointer
 * device when the controller is ready.
 */
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
	/* partial render mode: one area at a time goes to the panel */
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

/**
 * @brief The LVGL task.
 *
 * @param[in] argument Unused.
 *
 * Waits for the display and touch drivers to come up (they run from later init calls), then
 * loops: drain the request queue and run each callback, advance a pending flush, let
 * lv_timer_handler() do the rendering, and sleep for its delay. While a flush is still on the
 * wire the delay is capped at 1 ms so the end of the transfer is noticed promptly, and the
 * overall sleep is clamped to 1..20 ms. A task that cannot bring the display up records the
 * error and deletes itself.
 */
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
			/* run everything other tasks posted before the next render pass */
			request.callback(request.argument);
		}
		if (port.flush_pending) {
			(void)lvgl_flush_finish();
		}
		/* render and dispatch the LVGL timers */
		delay_ms = lv_timer_handler();
		/* keep the delay short while the panel still holds a buffer */
		if (port.flush_pending && delay_ms > 1U) {
			delay_ms = 1U; /* keep the end of the wire in sight */
		}
		msleep((int)(delay_ms < 1U ? 1U : (delay_ms > 20U ? 20U : delay_ms)));
	}
}

/**
 * @brief Queue a function to run in the LVGL task.
 *
 * @param[in] callback Function to run; may not be NULL.
 * @param[in] argument Opaque argument passed to @p callback.
 * @return 0 on success, -EINVAL for a NULL callback, -EAGAIN while LVGL is not up yet, or the
 *         queue's error.
 *
 * LVGL is not thread-safe, so other tasks reach it only through this queue.
 */
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

/**
 * @brief Report whether the LVGL task is ready.
 *
 * @return True once the display is set up.
 */
bool lvgl_port_ready(void)
{
	return port.ready;
}

/**
 * @brief Return the first error seen by the port.
 *
 * @return The recorded negative errno, or 0.
 */
int lvgl_port_last_error(void)
{
	return port.error;
}

/**
 * @brief Init call that creates the request queue and the LVGL task.
 *
 * @return 0 on success, -ENOMEM when the queue or the task cannot be created.
 *
 * The panel driver runs at the same init level but a later priority, so a short sleep covers
 * the usual ordering; the task itself retries opening the display either way.
 */
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
/**
 * @brief Convert a transfer into KiB/s.
 *
 * @param[in] bytes      Bytes per frame.
 * @param[in] frames     Number of frames.
 * @param[in] elapsed_us Elapsed microseconds; zero is treated as one to avoid a division by
 *                       zero.
 * @return The rate in KiB/s.
 */
static uint64_t bench_rate_kib(size_t bytes, uint32_t frames, uint64_t elapsed_us)
{
	return (uint64_t)bytes * frames * 1000000U / 1024U / (elapsed_us == 0U ? 1U : elapsed_us);
}

/**
 * @brief Wait for the flush to finish, for the benchmark.
 *
 * Polls once per millisecond until lvgl_flush_finish() reports the wire idle.
 */
static void bench_wait_idle(void)
{
	while (!lvgl_flush_finish()) {
		msleep(1U);
	}
}

/**
 * @brief Redraw the active screen a number of times.
 *
 * @param[in] frames Number of frames.
 * @return The elapsed time in microseconds.
 *
 * Each frame invalidates the whole screen, forces a refresh and waits for the wire, so the
 * measurement includes rendering and transfer.
 */
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

/**
 * @brief Measure the parts of a frame, one stage at a time.
 *
 * @param[in] argument Unused; the function is posted to the LVGL task.
 *
 * Runs, in order: a memory copy, a raw pixel write to the panel, a plain full-screen image
 * redraw, the same image blended at half opacity, and the real UI. For the last one the flush
 * counters are reset first, so the wire time can be split out of the wall time. The two large
 * buffers come from the heap and are freed on every path.
 */
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

/**
 * @brief "lvgl": print flush statistics, reset them, or run the benchmark.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0, or the negative errno from lvgl_port_post().
 *
 * With no argument the accumulated flush count, pixels, total and worst transfer time, and the
 * last error are printed. "reset" clears the counters and the error; "bench" posts the
 * benchmark to the LVGL task.
 */
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
