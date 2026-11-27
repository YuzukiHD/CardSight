/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file sunxi_display.h
 * @brief The panel: its size and pixel format, and the rectangle writes the UI flushes through.
 */

#ifndef CARDSIGHT_SUNXI_DISPLAY_H
#define CARDSIGHT_SUNXI_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Geometry and pixel format of the panel.
 */
struct sunxi_display_info {
	uint16_t width, height; /**< Panel size in pixels. */
	uint16_t fb_width, fb_height; /**< Frame buffer size in pixels. */
	uint32_t stride; /**< Bytes per frame buffer row. */
	uint8_t bytes_per_pixel; /**< Bytes per pixel; the panel uses 3 (RGB888). */
	bool pixels_swapped; /**< Whether pixel bytes are swapped for the panel; false on this board. */
};

/**
 * @brief Whether the display subsystem probed its panel and can accept writes.
 *
 * @return true once the panel is up.
 */
bool sunxi_display_ready(void);
/**
 * @brief Copy the published panel geometry out.
 *
 * @param[out] info Receives the geometry and pixel format.
 * @return 0 on success, or -ENODEV when the panel is not ready.
 */
int sunxi_display_get_info(struct sunxi_display_info *info);

/**
 * @brief Copy a rectangle to the panel.
 *
 * @details @p buffer holds RGB888 pixels in the panel byte order (B, G, R on this board;
 * see @c bytes_per_pixel and @c pixels_swapped). The window command is sent first, then the pixels go out over SPI; when
 * @p pitch equals @p width the whole rectangle is one transfer, otherwise each row is copied
 * separately.
 *
 * @param[in] x_pos  Left edge in panel pixels.
 * @param[in] y_pos  Top edge in panel pixels.
 * @param[in] width  Rectangle width in pixels.
 * @param[in] height Rectangle height in pixels.
 * @param[in] pitch  Source row width in pixels, including padding.
 * @param[in] buffer Source pixels, @p pitch pixels wide.
 * @return 0 on success, or -EINVAL when the panel is not ready or the rectangle is out of
 * bounds.
 */
int sunxi_display_write(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height,
	uint16_t pitch, const void *buffer);

/**
 * @brief Start a rectangle write without waiting for the wire.
 *
 * @details The window command goes out at once and the second hart pushes the pixels. Poll
 * sunxi_display_write_done() before reusing @p buffer. Only packed rectangles
 * (@p pitch == @p width) are supported.
 *
 * @param[in] x_pos  Left edge in panel pixels.
 * @param[in] y_pos  Top edge in panel pixels.
 * @param[in] width  Rectangle width in pixels.
 * @param[in] height Rectangle height in pixels.
 * @param[in] pitch  Source row width in pixels; must equal @p width.
 * @param[in] buffer Source pixels.
 * @return 0 on success, or -EINVAL for an unsupported or out-of-bounds rectangle.
 */
int sunxi_display_write_async(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height,
	uint16_t pitch, const void *buffer);

/**
 * @brief Report whether the asynchronous write has finished.
 *
 * @param[out] result Receives the write result when the transfer completed.
 * @return true once, when a completed transfer is reported; false while it runs.
 */
bool sunxi_display_write_done(int *result);

/** @brief Block until the panel is quiet again (the next window command needs it). */
void sunxi_display_wait(void);

#endif /* CARDSIGHT_SUNXI_DISPLAY_H */
