/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file spi-sunxi.h
 * @brief Board description and interface of the polled sunxi SPI controller.
 *
 * One controller (SPI1) is present; the bus description is a board array and
 * lives with the board. Only the write path is used.
 */

#ifndef CARDSIGHT_SPI_H
#define CARDSIGHT_SPI_H

#include <stddef.h>
#include <stdint.h>

#include <board/cardsight_board.h>

/**
 * @brief Description of one SPI controller and the pins that carry it.
 */
struct sunxi_spi_config {
	/** @brief MMIO base address of the controller. */
	uintptr_t base;
	/** @brief CCU offset of the module clock register (0x944 on this SoC). */
	uint32_t module_clock_register;
	/** @brief CCU register holding the bus clock gate and reset (CCU + 0x96c). */
	uintptr_t gate_address;
	/** @brief Bit of @c gate_address that gates the bus clock. */
	uint8_t gate_bit;
	/** @brief Bit of @c gate_address that controls the module reset. */
	uint8_t reset_bit;
	/** @brief Requested SCK frequency in Hz. */
	uint32_t frequency;
	/** @brief SCK pin description. */
	struct sunxi_pin clock;
	/** @brief MOSI pin description. */
	struct sunxi_pin mosi;
	/** @brief Chip-select pin description. */
	struct sunxi_pin cs;
};

/** @brief Board instance of the SPI1 controller. */
extern const struct sunxi_spi_config sunxi_spi1;

/**
 * @brief Initialise the controller: pins, module clock and the mode 0 write path.
 *
 * @param[in] config Board description of the controller.
 * @return 0 on success, -EINVAL when @p config is NULL, or -ETIMEDOUT when
 *         the FIFO reset never completes.
 */
int sunxi_spi_init(const struct sunxi_spi_config *config);

/**
 * @brief Send @p length bytes on MOSI; the chip select is driven by the controller.
 *
 * @param[in] buffer Bytes to send.
 * @param[in] length Number of bytes to send.
 * @return 0 on success, -EINVAL for bad arguments, -EIO on a controller error,
 *         or -ETIMEDOUT when the transfer does not finish within one second.
 */
int sunxi_spi_write(const void *buffer, size_t length);

#endif /* CARDSIGHT_SPI_H */
