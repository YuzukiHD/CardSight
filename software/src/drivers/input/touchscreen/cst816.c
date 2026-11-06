/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The touch controller of the board: a CST816 on I2C0 (400 kHz), polled. The LVGL input device
 * reads one report per call; no interrupt or worker task is needed for that.
 */

#define LOG_MODULE "cst816"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <hal/kernel.h>
#include <init.h>
#include <log.h>

#include "drivers/drivers.h"
#include "drivers/i2c/i2c-sunxi.h"

#define CST816_ADDRESS	     0x15U
#define CST816_GESTURE	     0x01U
#define CST816_CHIP_ID	     0xa7U
#define CST816_IRQ_CONTROL   0xfaU
#define CST816_DISABLE_SLEEP 0xfeU

static const struct sunxi_pin touch_reset = { .port = 4, .pin = 2, .function = 0 };

static bool touch_ready;

bool sunxi_touch_ready(void)
{
	return touch_ready;
}

/* One report: x, y and whether the panel is touched. */
int sunxi_touch_read(uint16_t *x_pos, uint16_t *y_pos, bool *pressed)
{
	const uint8_t reg = CST816_GESTURE;
	uint8_t report[6];
	int result;

	if (!touch_ready) {
		return -ENODEV;
	}
	result = sunxi_i2c_write_read(CST816_ADDRESS, &reg, 1U, report, sizeof(report));
	if (result != 0) {
		return result;
	}
	*x_pos = (uint16_t)(((report[2] & 0x0fU) << 8) | report[3]);
	*y_pos = (uint16_t)(((report[4] & 0x0fU) << 8) | report[5]);
	*pressed = (report[1] & 0x0fU) != 0U && (report[2] >> 6) != 1U;
	return 0;
}

static int touch_init(void)
{
	const uint8_t reg = CST816_CHIP_ID;
	uint8_t identity[3] = { 0 };
	uint8_t command[2];
	int result;

	result = sunxi_i2c_init(&sunxi_i2c0);
	if (result != 0) {
		return result;
	}
	sunxi_gpio_set_output(&touch_reset, 0);
	msleep(10U);
	sunxi_gpio_set_output(&touch_reset, 1);
	msleep(120U);
	if (sunxi_i2c_write_read(CST816_ADDRESS, &reg, 1U, identity, sizeof(identity)) != 0 ||
	    identity[0] == 0x00U || identity[0] == 0xffU) {
		pr_warn("no CST816 on I2C0\n");
		return -ENODEV;
	}
	command[0] = CST816_DISABLE_SLEEP;
	command[1] = 0x01U;
	(void)sunxi_i2c_write(CST816_ADDRESS, command, sizeof(command));
	command[0] = CST816_IRQ_CONTROL;
	command[1] = 0x60U;
	(void)sunxi_i2c_write(CST816_ADDRESS, command, sizeof(command));
	touch_ready = true;
	pr_info("CST816: chip %02x, project %02x, firmware %02x\n", identity[0], identity[1], identity[2]);
	return 0;
}
INITCALL(2, 25, touch_init);
