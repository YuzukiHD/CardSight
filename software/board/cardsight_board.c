/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file cardsight_board.c
 * @brief The board description: every number a driver needs, as plain constant structures.
 *
 * UART0 is the console: PH13 (TX) and PH14 (RX) on function 6, clocked from the 24 MHz HOSC and
 * gated at CCU + 0x90c (bit 0, reset in bit 16). The PLIC source of a peripheral is its CLIC id
 * minus the 16 the C907's PLIC adds; UART0 has CLIC id 18, so source 2.
 */

#include <board/cardsight_board.h>

#include <hal/io.h>
#include <init.h>

#include "drivers/display/panel-st7789.h"
#include "drivers/i2c/i2c-sunxi.h"
#include "drivers/spi/spi-sunxi.h"

/**
 * @brief Raise the PSRAM module clock from /4 to /3 of the peripheral PLL.
 *
 * @details The PSRAM module clock comes from the 400 MHz peripheral PLL in integer steps: the
 * loader leaves it at /4 (100 MHz); /3 gives 133 MHz, measured at 31 MB/s against 25 MB/s. The
 * divider field is written while the clock gate is closed, then the original gate state is
 * restored, because the second write starts from the value read before the change.
 *
 * @return 0; init calls ignore the value.
 */
static int board_psram_clock(void)
{
	uintptr_t clock_reg = SUNXI_CCU_BASE + 0x850U;
	uint32_t value = readl(clock_reg);

	writel(value & ~(1U << 31), clock_reg);
	writel((value & ~0xfU) | 2U, clock_reg);
	return 0;
}
INITCALL(1, 5, board_psram_clock);

/**
 * @brief The console UART: UART0, 115200 baud, pins PH13 and PH14.
 */
const struct sunxi_uart_config sunxi_uart0 = {
    .base = 0x02500000UL,
    .baud = 115200U,
    .clock_hz = 24000000U,
    .irq = 2U,
    .tx = {.port = 7, .pin = 13, .function = 6},
    .rx = {.port = 7, .pin = 14, .function = 6},
    .gate_address = SUNXI_CCU_BASE + 0x090cU,
    .reset_address = SUNXI_CCU_BASE + 0x090cU,
    .gate_bit = 0U,
    .reset_bit = 16U,
};

#include "drivers/mmc/host/sunxi-mmc.h"

/**
 * @brief The card slot: SMHC0 at 0x04020000.
 *
 * The built-in clock unit register is CCU + 0x830 (mux bits 27:24, P 9:8, M-1 3:0, gate 31),
 * and the bus clock and reset are in CCU + 0x84c. The pins are port F function 2; PF6 is the
 * card detect switch, pulled up and low when a card is in.
 */
const struct sunxi_mmc_config sunxi_mmc0 = {
    .base = 0x04020000UL,
    .gate_address = SUNXI_CCU_BASE + 0x084cU,
    .gate_bit = 0U,
    .reset_bit = 16U,
    .clock_register = 0x0830U,
    .max_clock = 50000000U,
    .power_delay_ms = 10U,
    /* the clock is driven at full strength, the command and data lines carry
       pull-ups */
    .clock = {.port = 5, .pin = 2, .function = 2, .drive = 3},
    .command = {.port = 5, .pin = 3, .function = 2, .pull = 1, .drive = 3},
    .data =
        {
            {.port = 5, .pin = 1, .function = 2, .pull = 1, .drive = 3},
            {.port = 5, .pin = 0, .function = 2, .pull = 1, .drive = 3},
            {.port = 5, .pin = 5, .function = 2, .pull = 1, .drive = 3},
            {.port = 5, .pin = 4, .function = 2, .pull = 1, .drive = 3},
        },
    .card_detect = {.port = 5, .pin = 6, .function = 0, .pull = 1},
};

/**
 * @brief The panel's SPI bus: SPI1 at 0x04026000.
 *
 * The module clock is in CCU + 0x944; the bus clock and reset are in CCU + 0x96c (bit 1, reset
 * in bit 17). SCK is 60 MHz; PD1, PD2 and PD3 carry clock, MOSI and chip select on function 6,
 * driven at strength 3.
 */
const struct sunxi_spi_config sunxi_spi1 = {
    .base = 0x04026000UL,
    .module_clock_register = 0x0944U,
    .gate_address = SUNXI_CCU_BASE + 0x096cU,
    .gate_bit = 1U,
    .reset_bit = 17U,
    .frequency = 75000000U,
    .clock = {.port = 3, .pin = 1, .function = 6, .drive = 3},
    .mosi = {.port = 3, .pin = 2, .function = 6, .drive = 3},
    .cs = {.port = 3, .pin = 3, .function = 6, .drive = 3},
};

/**
 * @brief The ST7789 panel on SPI1: 240x284 pixels, BGR order, mirrored horizontally.
 *
 * The data/command line is PD4 and the reset line PD5, both plain GPIO outputs.
 */
const struct sunxi_panel_config sunxi_panel0 = {
    .width = 240U,
    .height = 284U,
    .bgr = true,
    .mirror_x = true,
    .command = {.port = 3, .pin = 4, .function = 0},
    .reset = {.port = 3, .pin = 5, .function = 0},
    .spi = &sunxi_spi1,
};

/**
 * @brief The touch controller's I2C bus: TWI0 at 0x02502000.
 *
 * The bus clock and reset are in CCU + 0x91c (bit 0, reset in bit 16); the clock runs at
 * 400 kHz from the 24 MHz source. SCL and SDA are PE4 and PE5 on function 8.
 */
const struct sunxi_i2c_config sunxi_i2c0 = {
    .base = 0x02502000UL,
    .gate_address = SUNXI_CCU_BASE + 0x091cU,
    .gate_bit = 0U,
    .reset_bit = 16U,
    .clock_register_value = 0x28U,
    .scl = {.port = 4, .pin = 4, .function = 8},
    .sda = {.port = 4, .pin = 5, .function = 8},
};
