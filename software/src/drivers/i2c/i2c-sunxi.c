/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file i2c-sunxi.c
 * @brief The sunxi TWI controller, polled: start and repeated start, one
 *        address byte, then data.
 *
 * Only register transactions are used on this board, so there is no
 * interrupt path. Each phase writes the control register to launch it, then
 * waits for the controller's interrupt flag and compares the status code
 * against the value the protocol expects at that point.
 */

#define LOG_MODULE "i2c"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <hal/io.h>
#include <log.h>

#include "arch/arch.h"
#include "drivers/drivers.h"
#include "drivers/i2c/i2c-sunxi.h"

/** @brief Data register: byte to send or byte just received. */
#define TWI_DATA   0x08U
/** @brief Control register: bus enable, start/stop, ACK and interrupt flag. */
#define TWI_CTL	   0x0cU
/** @brief Status register; the low byte holds the bus state code. */
#define TWI_STATUS 0x10U
/** @brief Clock register: bus timing derived from the source clock. */
#define TWI_CLK	   0x14U
/** @brief Software reset register. */
#define TWI_SRST   0x18U
/** @brief Enhanced feature register, cleared before a transfer. */
#define TWI_EFT	   0x1cU

/** @brief Control bit: return ACK after a received byte. */
#define CTL_ACK	   (1U << 2)
/** @brief Control bit: interrupt flag, written as 1 to clear it. */
#define CTL_INTFLG (1U << 3)
/** @brief Control bit: issue a stop condition. */
#define CTL_STP	   (1U << 4)
/** @brief Control bit: issue a start condition. */
#define CTL_STA	   (1U << 5)
/** @brief Control bit: enable the bus. */
#define CTL_BUSEN  (1U << 6)

/** @brief Status: start condition transmitted. */
#define ST_START	0x08U
/** @brief Status: repeated start condition transmitted. */
#define ST_RESTART	0x10U
/** @brief Status: address plus write bit transmitted, ACK received. */
#define ST_ADDR_W	0x18U
/** @brief Status: data byte transmitted, ACK received. */
#define ST_DATA_W	0x28U
/** @brief Status: address plus read bit transmitted, ACK received. */
#define ST_ADDR_R	0x40U
/** @brief Status: data byte received, ACK returned. */
#define ST_DATA_R_ACK	0x50U
/** @brief Status: data byte received, NACK returned (last byte). */
#define ST_DATA_R_NACK	0x58U
/** @brief Status: bus idle. */
#define ST_IDLE		0xf8U

/** @brief Controller description set by @ref sunxi_i2c_init; NULL until then. */
static const struct sunxi_i2c_config *i2c_config;

/**
 * @brief Wait until the controller reports the expected bus state.
 *
 * @details Polls the interrupt flag; once it is set, compares the low byte of
 *          the status register against @p expected. The four error states the
 *          controller reports (0x00, 0x20, 0x30 and 0x48) end the wait with
 *          -EIO instead of running into the timeout. The flag is left set
 *          here; the caller clears it when it launches the next phase.
 *
 * @param[in] expected Status code the current phase should produce.
 * @return 0 when the expected state is seen, -EIO on a bus error, or
 *         -ETIMEDOUT after a fixed number of polls.
 */
static int twi_wait(uint32_t expected)
{
	for (uint32_t spins = 0U; spins < 200000U; spins++) {
		if ((readl(i2c_config->base + TWI_CTL) & CTL_INTFLG) != 0U) {
			uint32_t status = readl(i2c_config->base + TWI_STATUS) & 0xffU;

			if (status == expected) {
				return 0;
			}
			if (status == 0x00U || status == 0x20U || status == 0x30U || status == 0x48U) {
				return -EIO;
			}
		}
	}
	return -ETIMEDOUT;
}

/**
 * @brief Issue a start or repeated start condition.
 *
 * @details A fresh start first clears the enhanced feature register, pulses
 *          the software reset and waits for the reset bit to self-clear and
 *          the bus to report idle, then enables the bus. Both variants clear
 *          the interrupt flag and any pending stop, set the start bit, and
 *          for a repeated start also write the interrupt flag so the stale
 *          state from the previous phase is acknowledged. The wait expects
 *          0x08 for a start and 0x10 for a repeated start.
 *
 * @param[in] repeated true to issue a repeated start on a live bus.
 * @return 0 on success, or the error from @ref twi_wait.
 */
static int twi_start(bool repeated)
{
	uint32_t control;

	if (!repeated) {
		writel(0U, i2c_config->base + TWI_EFT);
		writel(1U, i2c_config->base + TWI_SRST);
		for (uint32_t spins = 0U; spins < 100000U; spins++) {
			if ((readl(i2c_config->base + TWI_SRST) & 1U) == 0U &&
			    (readl(i2c_config->base + TWI_STATUS) & 0xffU) == ST_IDLE) {
				break;
			}
		}
		writel(CTL_BUSEN, i2c_config->base + TWI_CTL);
	}
	control = readl(i2c_config->base + TWI_CTL) & ~(CTL_INTFLG | CTL_STP);
	writel(control | CTL_STA | (repeated ? CTL_INTFLG : 0U), i2c_config->base + TWI_CTL);
	return twi_wait(repeated ? ST_RESTART : ST_START);
}

/**
 * @brief Send the address byte and wait for the controller to acknowledge it.
 *
 * @details The 7-bit address is shifted left and the read/write bit
 *          appended. Writing the data register starts the transfer; the
 *          interrupt flag is cleared afterwards to arm the wait. The expected
 *          status is 0x40 for a read and 0x18 for a write.
 *
 * @param[in] address 7-bit device address.
 * @param[in] read    true to send the address with the read bit set.
 * @return 0 on success, or the error from @ref twi_wait.
 */
static int twi_address(uint16_t address, bool read)
{
	writel(((uint32_t)address << 1) | (read ? 1U : 0U), i2c_config->base + TWI_DATA);
	writel(readl(i2c_config->base + TWI_CTL) | CTL_INTFLG, i2c_config->base + TWI_CTL);
	return twi_wait(read ? ST_ADDR_R : ST_ADDR_W);
}

/**
 * @brief Issue a stop condition and wait for the bus to go idle.
 *
 * @details Sets the stop bit together with the interrupt flag. The controller
 *          clears the stop bit itself once the condition is on the wire; the
 *          status must then report idle. A short delay after the transition
 *          gives the bus time to settle before the next start. The result is
 *          ignored by the callers, which report the earlier failure instead.
 *
 * @return 0 when the stop completed, or -ETIMEDOUT after a fixed number of polls.
 */
static int twi_stop(void)
{
	writel(readl(i2c_config->base + TWI_CTL) | CTL_STP | CTL_INTFLG, i2c_config->base + TWI_CTL);
	for (uint32_t spins = 0U; spins < 100000U; spins++) {
		if ((readl(i2c_config->base + TWI_CTL) & CTL_STP) == 0U &&
		    (readl(i2c_config->base + TWI_STATUS) & 0xffU) == ST_IDLE) {
			cs_udelay(5U);
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/**
 * @brief Send a run of bytes after the address phase.
 *
 * @details Each iteration stores one byte, clears the interrupt flag to arm
 *          the wait, and waits for the transmit-data-ACK status. Any failure
 *          aborts the run; the caller still issues the stop.
 *
 * @param[in] data   Bytes to send.
 * @param[in] length Number of bytes to send.
 * @return 0 on success, or -EIO when a byte is not acknowledged.
 */
static int twi_write(const uint8_t *data, size_t length)
{
	for (size_t index = 0U; index < length; index++) {
		writel(data[index], i2c_config->base + TWI_DATA);
		writel(readl(i2c_config->base + TWI_CTL) | CTL_INTFLG, i2c_config->base + TWI_CTL);
		if (twi_wait(ST_DATA_W) != 0) {
			return -EIO;
		}
	}
	return 0;
}

/**
 * @brief Receive a run of bytes after the address phase.
 *
 * @details ACK is returned for every byte except the last, which is NACKed
 *          so the device releases the bus; the control register is rewritten
 *          before each byte to switch that bit. After the flag is cleared the
 *          wait expects 0x50 for an ACKed byte and 0x58 for the final NACKed
 *          one, then the data register is read.
 *
 * @param[out] data   Buffer receiving the bytes.
 * @param[in]  length Number of bytes to receive.
 * @return 0 on success, or -EIO when the status does not match.
 */
static int twi_read(uint8_t *data, size_t length)
{
	for (size_t index = 0U; index < length; index++) {
		uint32_t control = readl(i2c_config->base + TWI_CTL);

		if (index + 1U < length) {
			control |= CTL_ACK;
		} else {
			control &= ~CTL_ACK;
		}
		writel(control | CTL_INTFLG, i2c_config->base + TWI_CTL);
		if (twi_wait(index + 1U < length ? ST_DATA_R_ACK : ST_DATA_R_NACK) != 0) {
			return -EIO;
		}
		data[index] = (uint8_t)readl(i2c_config->base + TWI_DATA);
	}
	return 0;
}

/**
 * @brief Initialise the TWI controller described by the board.
 *
 * @details Stores the description for later transactions, muxes the SCL and
 *          SDA pins, enables the bus clock and releases the reset through the
 *          CCU helper, then programs the clock register with the board's
 *          pre-computed timing value. The reset and gate bits live in the
 *          same CCU register on this SoC, so @c gate_address is passed as both.
 *
 * @param[in] config Board description of the controller.
 * @return 0 on success, or -EINVAL when @p config is NULL.
 */
int sunxi_i2c_init(const struct sunxi_i2c_config *config)
{
	if (config == NULL) {
		return -EINVAL;
	}
	i2c_config = config;

	sunxi_pinctrl_set(&config->scl);
	sunxi_pinctrl_set(&config->sda);
	sunxi_ccu_enable(config->gate_address, config->gate_bit, config->gate_address, config->reset_bit);
	writel(config->clock_register_value, config->base + TWI_CLK);
	return 0;
}

/**
 * @brief Write a run of bytes to a device in a single transaction.
 *
 * @details Runs start, address, then data, stopping at the first failing
 *          phase. The stop condition is always issued, even on failure, so
 *          the bus is returned to idle; a failure from the stop itself is
 *          discarded in favour of the phase error.
 *
 * @param[in] address 7-bit device address.
 * @param[in] data    Bytes to send.
 * @param[in] length  Number of bytes to send.
 * @return 0 on success. -EINVAL when there is no controller or the buffers
 *         are invalid; otherwise the negative errno of the failing phase.
 */
int sunxi_i2c_write(uint8_t address, const uint8_t *data, size_t length)
{
	int result;

	if (i2c_config == NULL || data == NULL || length == 0U) {
		return -EINVAL;
	}
	result = twi_start(false);
	if (result == 0) {
		result = twi_address(address, false);
	}
	if (result == 0) {
		result = twi_write(data, length);
	}
	(void)twi_stop();
	return result;
}

/**
 * @brief Write a register address, then read a reply with a repeated start.
 *
 * @details Runs the phases in order: start, address write, optional write
 *          bytes, repeated start, address read, then the reply bytes. Each
 *          phase only runs when the previous one succeeded. The stop
 *          condition is issued unconditionally so the bus is left idle, and
 *          its own error is discarded.
 *
 * @param[in]  address      7-bit device address.
 * @param[in]  write        Bytes sent before the repeated start; may be NULL
 *                          only when @p write_length is 0.
 * @param[in]  write_length Number of bytes in @p write.
 * @param[out] read         Buffer receiving the reply; may be NULL only when
 *                          @p read_length is 0.
 * @param[in]  read_length  Number of bytes to read.
 * @return 0 on success, -EINVAL for bad arguments, or the negative errno of
 *         the failing phase.
 */
int sunxi_i2c_write_read(uint8_t address, const uint8_t *write, size_t write_length,
	uint8_t *read, size_t read_length)
{
	int result;

	if (i2c_config == NULL || (write == NULL && write_length != 0U) ||
	    (read == NULL && read_length != 0U)) {
		return -EINVAL;
	}
	result = twi_start(false);
	if (result == 0) {
		result = twi_address(address, false);
	}
	if (result == 0) {
		result = twi_write(write, write_length);
	}
	if (result == 0) {
		result = twi_start(true);
	}
	if (result == 0) {
		result = twi_address(address, true);
	}
	if (result == 0) {
		result = twi_read(read, read_length);
	}
	(void)twi_stop();
	return result;
}
