/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file drivers.h
 * @brief What the drivers offer the rest of the platform.
 *
 * Declares the small platform-facing driver entry points: CCU gate/reset
 * control, pin mux and GPIO access, the polled CST816 touch panel, and the
 * DesignWare console UART. Implementations live in the sibling source
 * directories.
 */

#ifndef CARDSIGHT_DRIVERS_H
#define CARDSIGHT_DRIVERS_H

#include <stdbool.h>
#include <stdint.h>
#include <board/cardsight_board.h>

/**
 * @brief Enable the module clock and release its reset.
 *
 * @details Implemented in clk/sunxi-ccu.c. Asserts the reset first, switches
 *          the bus/module clock gate on, then releases the reset so the block
 *          never runs while its clock is off.
 *
 * @param[in] gate_address Address of the CCU gate register, or 0 to skip the clock.
 * @param[in] gate_bit     Bit position of the module's gate in @p gate_address.
 * @param[in] reset_address Address of the CCU reset register, or 0 to skip the reset.
 * @param[in] reset_bit    Bit position of the module's reset in @p reset_address.
 */
void sunxi_ccu_enable(uintptr_t gate_address, uint32_t gate_bit, uintptr_t reset_address,
	uint32_t reset_bit);

/**
 * @brief Mux a pin to its function, optionally setting pull and drive strength.
 *
 * @details Implemented in pinctrl/pinctrl-sunxi.c. Writes the 4-bit function
 *          field and, when non-zero, the pull and drive fields of the
 *          requested bank.
 *
 * @param[in] pin Pin description from the board file.
 */
void sunxi_pinctrl_set(const struct sunxi_pin *pin);

/**
 * @brief Mux a pin as a plain output and drive its data bit.
 *
 * @param[in] pin   Pin description from the board file.
 * @param[in] value Non-zero drives the pin high, zero drives it low.
 */
void sunxi_gpio_set_output(const struct sunxi_pin *pin, int value);

/**
 * @brief Read the input data bit of a pin.
 *
 * @param[in] pin Pin description from the board file.
 * @return The sampled level (0 or 1), or -1 when the pin's bank does not exist.
 */
int sunxi_gpio_get(const struct sunxi_pin *pin);

/**
 * @brief Report whether the capacitive touch panel probed successfully.
 *
 * @details Implemented in cst816.c; the panel sits on I2C0 and is polled.
 *
 * @return true when touch reports can be read, false otherwise.
 */
bool sunxi_touch_ready(void);

/**
 * @brief Read one touch report from the panel.
 *
 * @param[out] x_pos   Panel X coordinate.
 * @param[out] y_pos   Panel Y coordinate.
 * @param[out] pressed Whether a finger is currently reported as down.
 * @return 0 on success, -ENODEV when the panel was not found, or another
 *         negative errno from the I2C transfer.
 */
int sunxi_touch_read(uint16_t *x_pos, uint16_t *y_pos, bool *pressed);

/**
 * @brief Configure the console UART: clocks, pins and line settings.
 *
 * @details Implemented in tty/serial/uart-dw.c; TX stays polled so it works
 *          from the first instruction.
 *
 * @param[in] config Board description of the UART.
 * @return 0 on success, or -EINVAL when @p config, its base or its baud rate is invalid.
 */
int sunxi_uart_init(const struct sunxi_uart_config *config);

/**
 * @brief Transmit one character, waiting for the transmitter to make room.
 *
 * @param[in] character Character to send.
 */
void sunxi_uart_putc(char character);

/**
 * @brief Receive one character from the interrupt-fed queue.
 *
 * @param[in] timeout_ms How long to wait for a character, in milliseconds.
 * @return The character (0..255) on success, -ENODEV when RX was never
 *         started, or -EAGAIN on timeout.
 */
int sunxi_uart_getc(int timeout_ms);

/**
 * @brief Create the RX queue and start interrupt-driven reception.
 *
 * @return 0 on success, -ENODEV when the UART was not initialised or has no
 *         IRQ, -ENOMEM when the queue cannot be created, or -EINVAL when the
 *         interrupt cannot be connected.
 */
int sunxi_uart_start_rx(void);

#endif /* CARDSIGHT_DRIVERS_H */
