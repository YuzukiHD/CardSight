/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The TWI controller of the board, polled. One controller (I2C0) is present; the bus
 * description is a board object and lives with the board.
 */

#ifndef CARDSIGHT_I2C_H
#define CARDSIGHT_I2C_H

#include <stddef.h>
#include <stdint.h>

#include <board/cardsight_board.h>

struct sunxi_i2c_config {
	uintptr_t base;
	uintptr_t gate_address;		/* CCU gate/reset register */
	uint8_t gate_bit;
	uint8_t reset_bit;
	uint32_t clock_register_value;	/* TWI_CLK value: 0x28 is 400 kHz from 24 MHz */
	struct sunxi_pin scl;
	struct sunxi_pin sda;
};

extern const struct sunxi_i2c_config sunxi_i2c0;

/** Pins, clocks and the controller's clock register. */
int sunxi_i2c_init(const struct sunxi_i2c_config *config);

/** Write @p length bytes to the device at @p address in one transaction. */
int sunxi_i2c_write(uint8_t address, const uint8_t *data, size_t length);

/** Write the register address, then read @p read_length bytes, with a repeated start. */
int sunxi_i2c_write_read(uint8_t address, const uint8_t *write, size_t write_length,
	uint8_t *read, size_t read_length);

#endif /* CARDSIGHT_I2C_H */
