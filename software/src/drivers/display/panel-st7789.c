/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The panel of the board: an ST7789-compatible controller behind SPI1, four-wire SPI at
 * 60 MHz, data/command on PD4 and reset on PD5. The panel is write only: the controller's
 * receive path is not used.
 */

#define LOG_MODULE "display"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <hal/kernel.h>
#include <init.h>
#include <log.h>

#include "arch/arch.h"
#include "drivers/drivers.h"
#include "drivers/display/panel-st7789.h"
#include "drivers/spi/spi-sunxi.h"

#define ST7789_SLPOUT  0x11U
#define ST7789_COLMOD  0x3aU
#define ST7789_MADCTL  0x36U
#define ST7789_CASET   0x2aU
#define ST7789_RASET   0x2bU
#define ST7789_RAMWR   0x2cU
#define ST7789_DISPON  0x29U

#define MADCTL_MIRROR_X 0x40U
#define MADCTL_BGR	0x08U

static const struct sunxi_panel_config *panel_config;

struct panel_command {
	uint8_t command;
	uint8_t count;
	uint16_t delay_ms;
	uint8_t parameters[14];
};

static const struct panel_command panel_init[] = {
	{ ST7789_SLPOUT, 0U, 120U, { 0 } },
	{ ST7789_MADCTL, 1U, 0U, { MADCTL_MIRROR_X | MADCTL_BGR } },
	{ ST7789_COLMOD, 1U, 0U, { 0x05U } }, /* 16 bits per pixel */
	{ 0xb2U, 5U, 0U, { 0x0cU, 0x0cU, 0x00U, 0x33U, 0x33U } },
	{ 0xb7U, 1U, 0U, { 0x05U } },
	{ 0xbbU, 1U, 0U, { 0x21U } },
	{ 0xc0U, 1U, 0U, { 0x2cU } },
	{ 0xc2U, 1U, 0U, { 0x01U } },
	{ 0xc3U, 1U, 0U, { 0x15U } },
	{ 0xc6U, 1U, 0U, { 0x0fU } },
	{ 0xd0U, 1U, 0U, { 0xa7U } },
	{ 0xd0U, 2U, 0U, { 0xa4U, 0xa1U } },
	{ 0xd6U, 1U, 0U, { 0xa1U } },
	{ 0xe0U, 14U, 0U,
		{ 0xf0U, 0x05U, 0x0eU, 0x08U, 0x0aU, 0x17U, 0x39U, 0x54U, 0x4eU, 0x37U, 0x12U, 0x12U, 0x31U, 0x37U } },
	{ 0xe1U, 14U, 0U,
		{ 0xf0U, 0x10U, 0x14U, 0x0dU, 0x0bU, 0x05U, 0x39U, 0x44U, 0x4dU, 0x38U, 0x14U, 0x14U, 0x2eU, 0x35U } },
	{ 0xe4U, 3U, 0U, { 0x23U, 0x00U, 0x00U } },
	{ ST7789_DISPON, 0U, 0U, { 0 } },
	{ ST7789_RAMWR, 0U, 0U, { 0 } },
};

static int panel_command(uint8_t command, const uint8_t *parameters, size_t count)
{
	int result;

	sunxi_gpio_set_output(&panel_config->command, 0);
	result = sunxi_spi_write(&command, 1U);
	if (result != 0 || count == 0U) {
		return result;
	}
	sunxi_gpio_set_output(&panel_config->command, 1);
	return sunxi_spi_write(parameters, count);
}

int st7789_set_window(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t height)
{
	uint16_t column_start = x_pos;
	uint16_t column_end = (uint16_t)(x_pos + width - 1U);
	uint16_t row_start = y_pos;
	uint16_t row_end = (uint16_t)(y_pos + height - 1U);
	uint8_t columns[] = { (uint8_t)(column_start >> 8), (uint8_t)column_start,
		(uint8_t)(column_end >> 8), (uint8_t)column_end };
	uint8_t rows[] = { (uint8_t)(row_start >> 8), (uint8_t)row_start,
		(uint8_t)(row_end >> 8), (uint8_t)row_end };
	int result = panel_command(ST7789_CASET, columns, sizeof(columns));

	if (result == 0) {
		result = panel_command(ST7789_RASET, rows, sizeof(rows));
	}
	return result == 0 ? panel_command(ST7789_RAMWR, NULL, 0U) : result;
}

int st7789_write_pixels(const void *buffer, size_t length)
{
	sunxi_gpio_set_output(&panel_config->command, 1);
	return sunxi_spi_write(buffer, length);
}

int st7789_probe(const struct sunxi_panel_config *config)
{
	int result;

	if (config == NULL || config->spi == NULL) {
		return -EINVAL;
	}
	panel_config = config;
	result = sunxi_spi_init(config->spi);
	if (result != 0) {
		return result;
	}

	/* reset and initialise the panel */
	sunxi_gpio_set_output(&config->reset, 0);
	msleep(20U);
	sunxi_gpio_set_output(&config->reset, 1);
	msleep(120U);
	for (unsigned int index = 0U; index < ARRAY_SIZE(panel_init); index++) {
		const struct panel_command *step = &panel_init[index];

		if (panel_command(step->command, step->parameters, step->count) != 0) {
			return -EIO;
		}
		if (step->delay_ms != 0U) {
			msleep(step->delay_ms);
		}
	}
	pr_info("ST7789 on SPI1 at %u MHz, %ux%u\n", (unsigned)(config->spi->frequency / 1000000U),
		config->width, config->height);
	return 0;
}
