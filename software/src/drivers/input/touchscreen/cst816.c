/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file cst816.c
 * @brief The touch controller of the board: a CST816 on I2C0 (400 kHz), polled.
 *
 * The LVGL input device reads one report per call; no interrupt or worker
 * task is needed for that. Probe runs from an initcall, initialises I2C0 and
 * pulses the touch controller's reset line.
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

/** @brief 7-bit I2C address of the panel. */
#define CST816_ADDRESS	     0x15U
/** @brief Register of the gesture byte; a six-byte read returns one full report. */
#define CST816_GESTURE	     0x01U
/** @brief Register that returns the chip, project and firmware identifiers. */
#define CST816_CHIP_ID	     0xa7U
/** @brief Register selecting which events drive the interrupt pin. */
#define CST816_IRQ_CONTROL   0xfaU
/** @brief Register used to disable automatic sleep. */
#define CST816_DISABLE_SLEEP 0xfeU

/** @brief Reset line of the touch controller: PE2, driven as a plain GPIO. */
static const struct sunxi_pin touch_reset = { .port = 4, .pin = 2, .function = 0 };

/** @brief True once the panel answered its identity read during probe. */
static bool touch_ready;

/**
 * @brief Report whether the panel was found during probe.
 *
 * @return true when reports can be read from the panel.
 */
bool sunxi_touch_ready(void)
{
	return touch_ready;
}

/**
 * @brief Read one report: x, y and whether the panel is touched.
 *
 * @details Reads six bytes starting at the gesture register. The first byte
 *          is the gesture, the low nibble of the second is the number of
 *          touch points, and the remaining four bytes carry the coordinates
 *          with the high nibble taking the top four bits: report[2] bits 0..3
 *          with report[3], then report[4] bits 0..3 with report[5]. The press
 *          flag is set when at least one point is reported and the two-bit
 *          event field in report[2] bits 6..7 is not 1.
 *
 * @param[out] x_pos   Panel X coordinate.
 * @param[out] y_pos   Panel Y coordinate.
 * @param[out] pressed Whether the panel reports a touch.
 * @return 0 on success, -ENODEV before a successful probe, or the negative
 *         errno from the I2C transfer.
 */
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

/**
 * @brief Probe and configure the panel, then mark it ready.
 *
 * @details Initialises I2C0, drives the reset line low for 10 ms and releases
 *          it, waiting 120 ms for the controller to boot. It then reads the
 *          three identity bytes and treats an all-zero or all-ones first byte
 *          as a missing panel, which covers both no-ack and a floating bus.
 *          A found panel is kept awake with the disable-sleep command and the
 *          interrupt control register is set to 0x60; both writes are
 *          best-effort and their errors are ignored. On success the ready
 *          flag is set and the identifiers are logged.
 *
 * @return 0 on success, a negative errno from I2C setup, or -ENODEV when the
 *         panel does not answer with a plausible identity.
 */
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
/** @brief Probe the panel at initcall level 2, priority 25 (after I2C and GPIO). */
INITCALL(2, 25, touch_init);
