/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file i2c-sunxi.h
 * @brief Board description and interface of the polled sunxi TWI controller.
 *
 * One controller (I2C0) is present; its bus description is a board object and
 * lives with the board. Only register-style transactions are used, so the
 * driver has no interrupt path.
 */

#ifndef CARDSIGHT_I2C_H
#define CARDSIGHT_I2C_H

#include <stddef.h>
#include <stdint.h>

#include <board/cardsight_board.h>

/**
 * @brief Description of one TWI controller and the pins that carry it.
 */
struct sunxi_i2c_config {
	/** @brief MMIO base address of the TWI controller. */
	uintptr_t base;
	/** @brief CCU gate/reset register for the controller. */
	uintptr_t gate_address;
	/** @brief Bit of @c gate_address that gates the bus clock. */
	uint8_t gate_bit;
	/** @brief Bit of @c gate_address that controls the module reset. */
	uint8_t reset_bit;
	/** @brief TWI_CLK value: 0x28 is 400 kHz from a 24 MHz source clock. */
	uint32_t clock_register_value;
	/** @brief SCL pin description. */
	struct sunxi_pin scl;
	/** @brief SDA pin description. */
	struct sunxi_pin sda;
};

/** @brief Board instance of the I2C0 controller. */
extern const struct sunxi_i2c_config sunxi_i2c0;

/**
 * @brief Initialise the controller: pins, clocks and the clock register.
 *
 * @param[in] config Board description of the controller.
 * @return 0 on success, or -EINVAL when @p config is NULL.
 */
int sunxi_i2c_init(const struct sunxi_i2c_config *config);

/**
 * @brief Write @p length bytes to the device at @p address in one transaction.
 *
 * @param[in] address 7-bit device address.
 * @param[in] data    Bytes to send.
 * @param[in] length  Number of bytes to send.
 * @return 0 on success, -EINVAL for bad arguments, -EIO for a bus error, or
 *         -ETIMEDOUT when the controller does not complete.
 */
int sunxi_i2c_write(uint8_t address, const uint8_t *data, size_t length);

/**
 * @brief Write a register address, then read bytes back with a repeated start.
 *
 * @param[in]  address     7-bit device address.
 * @param[in]  write       Bytes sent before the repeated start.
 * @param[in]  write_length Number of bytes in @p write.
 * @param[out] read        Buffer receiving the reply.
 * @param[in]  read_length Number of bytes to read.
 * @return 0 on success, -EINVAL for bad arguments, -EIO for a bus error, or
 *         -ETIMEDOUT when the controller does not complete.
 */
int sunxi_i2c_write_read(uint8_t address, const uint8_t *write, size_t write_length,
	uint8_t *read, size_t read_length);

#endif /* CARDSIGHT_I2C_H */
