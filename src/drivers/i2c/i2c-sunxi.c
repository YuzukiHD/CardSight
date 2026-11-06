/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The sunxi TWI controller, polled: start and repeated start, one address byte, then data.
 * Only register transactions are used on this board, so there is no interrupt path.
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

#define TWI_DATA   0x08U
#define TWI_CTL	   0x0cU
#define TWI_STATUS 0x10U
#define TWI_CLK	   0x14U
#define TWI_SRST   0x18U
#define TWI_EFT	   0x1cU

#define CTL_ACK	   (1U << 2)
#define CTL_INTFLG (1U << 3)
#define CTL_STP	   (1U << 4)
#define CTL_STA	   (1U << 5)
#define CTL_BUSEN  (1U << 6)

#define ST_START	0x08U
#define ST_RESTART	0x10U
#define ST_ADDR_W	0x18U
#define ST_DATA_W	0x28U
#define ST_ADDR_R	0x40U
#define ST_DATA_R_ACK	0x50U
#define ST_DATA_R_NACK	0x58U
#define ST_IDLE		0xf8U

static const struct sunxi_i2c_config *i2c_config;

static int twi_wait(uint32_t expected)
{
	for (unsigned int spins = 0U; spins < 200000U; spins++) {
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

static int twi_start(bool repeated)
{
	uint32_t control;

	if (!repeated) {
		writel(0U, i2c_config->base + TWI_EFT);
		writel(1U, i2c_config->base + TWI_SRST);
		for (unsigned int spins = 0U; spins < 100000U; spins++) {
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

static int twi_address(uint16_t address, bool read)
{
	writel(((uint32_t)address << 1) | (read ? 1U : 0U), i2c_config->base + TWI_DATA);
	writel(readl(i2c_config->base + TWI_CTL) | CTL_INTFLG, i2c_config->base + TWI_CTL);
	return twi_wait(read ? ST_ADDR_R : ST_ADDR_W);
}

static int twi_stop(void)
{
	writel(readl(i2c_config->base + TWI_CTL) | CTL_STP | CTL_INTFLG, i2c_config->base + TWI_CTL);
	for (unsigned int spins = 0U; spins < 100000U; spins++) {
		if ((readl(i2c_config->base + TWI_CTL) & CTL_STP) == 0U &&
		    (readl(i2c_config->base + TWI_STATUS) & 0xffU) == ST_IDLE) {
			cs_udelay(5U);
			return 0;
		}
	}
	return -ETIMEDOUT;
}

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
