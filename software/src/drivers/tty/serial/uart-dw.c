/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file uart-dw.c
 * @brief The DesignWare APB UART (8250 compatible, 4-byte register stride) as
 *        the console.
 *
 * TX is polled so it works from the first instruction; RX is interrupt driven
 * once the scheduler runs and feeds a queue the shell reads.
 */

#include <hal/io.h>
#include <hal/kernel.h>

#include "drivers/drivers.h"
#include "arch/arch.h"

/** @brief Receive buffer / transmit holding register, or divisor low byte. */
#define RBR_THR_DLL 0x00U
/** @brief Interrupt enable register, or divisor high byte. */
#define IER_DLH	    0x04U
/** @brief Interrupt identification register, or FIFO control register. */
#define IIR_FCR	    0x08U
/** @brief Line control register (word length, stop bits, divisor latch). */
#define LCR	    0x0cU
/** @brief Modem control register. */
#define MCR	    0x10U
/** @brief Line status register (data ready, transmitter empty). */
#define LSR	    0x14U
/** @brief UART status register (busy indication). */
#define USR	    0x7cU

/** @brief LSR bit: received data ready. */
#define LSR_DR	 0x01U
/** @brief LSR bit: transmit holding register empty. */
#define LSR_THRE 0x20U
/** @brief IER bit: enable the received-data-available interrupt. */
#define IER_ERBFI 0x01U

/** @brief MMIO base of the console UART; zero before initialisation. */
static uintptr_t uart_base;
/** @brief PLIC source of the console UART; zero when unavailable. */
static uint32_t uart_irq;
/** @brief Queue of received characters, filled by the ISR and read by the shell. */
static QueueHandle_t rx_queue;

/**
 * @brief Configure the console UART and its pins.
 *
 * @details Validates the description, enables the bus clock and releases the
 *          reset, then muxes TX and RX. The divisor is rounded to the nearest
 *          integer of @c clock_hz/(16*baud). Initialisation asserts DTR and
 *          RTS, sets the divisor-latch bit in LCR, writes the high and low
 *          divisor bytes through the aliased registers, then selects 8N1 and
 *          enables and resets both FIFOs. The RX interrupt is deliberately
 *          not enabled here: polling TX must work before the scheduler starts,
 *          so RX is armed later by @ref sunxi_uart_start_rx.
 *
 * @param[in] config Board description of the UART.
 * @return 0 on success, or -EINVAL when @p config, its base or its baud is invalid.
 */
int sunxi_uart_init(const struct sunxi_uart_config *config)
{
	uint32_t divisor;

	if (config == NULL || config->base == 0U || config->baud == 0U) {
		return -EINVAL;
	}
	sunxi_ccu_enable(config->gate_address, config->gate_bit, config->reset_address, config->reset_bit);
	sunxi_pinctrl_set(&config->tx);
	sunxi_pinctrl_set(&config->rx);
	uart_base = config->base;
	uart_irq = config->irq;

	divisor = (config->clock_hz + 8U * config->baud) / (16U * config->baud);
	writel(0x3U, uart_base + MCR);
	writel(readl(uart_base + LCR) | 0x80U, uart_base + LCR); /* divisor latch access */
	writel(divisor >> 8, uart_base + IER_DLH);
	writel(divisor & 0xffU, uart_base + RBR_THR_DLL);
	writel(0x03U, uart_base + LCR);	    /* 8N1 */
	writel(0x07U, uart_base + IIR_FCR); /* enable and reset the FIFOs */
	return 0;
}

/**
 * @brief Send one character on the console, polling for room.
 *
 * @details Spins until the transmitter holding register reports empty, then
 *          stores the byte. Works before the scheduler is running and from any
 *          context, including the panic path. Does nothing before the UART is
 *          initialised.
 *
 * @param[in] character Character to send; only the low byte is transmitted.
 */
void sunxi_uart_putc(char character)
{
	if (uart_base == 0U) {
		return;
	}
	while ((readl(uart_base + LSR) & LSR_THRE) == 0U) {
	}
	writel((uint8_t)character, uart_base + RBR_THR_DLL);
}

/**
 * @brief Wait for one received character from the RX queue.
 *
 * @details Blocks on the FreeRTOS queue for at most @p timeout_ms
 *          milliseconds, converted to ticks. The queue is only created by
 *          @ref sunxi_uart_start_rx, so a missing queue means RX was never
 *          armed.
 *
 * @param[in] timeout_ms Maximum wait in milliseconds.
 * @return The character value on success, -ENODEV when RX was not started, or
 *         -EAGAIN when the wait times out.
 */
int sunxi_uart_getc(int timeout_ms)
{
	uint8_t character;

	if (rx_queue == NULL) {
		return -ENODEV;
	}
	if (xQueueReceive(rx_queue, &character, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
		return -EAGAIN;
	}
	return character;
}

/**
 * @brief RX interrupt handler: move received bytes into the queue.
 *
 * @details A busy-detect interrupt (IIR value 0x7) has no data to read and is
 *          acknowledged by reading the UART status register. The handler then
 *          drains the receive FIFO while the data-ready bit is set, pushing
 *          each byte into the queue with the ISR-safe send. A full queue
 *          drops the byte silently rather than blocking in interrupt context.
 *
 * @param[in] argument Unused handler argument.
 */
static void uart_isr(void *argument)
{
	uint8_t character;

	(void)argument;
	if ((readl(uart_base + IIR_FCR) & 0xfU) == 0x7U) {
		(void)readl(uart_base + USR); /* busy detect: cleared by reading USR */
	}
	while ((readl(uart_base + LSR) & LSR_DR) != 0U) {
		character = (uint8_t)readl(uart_base + RBR_THR_DLL);
		(void)xQueueSendFromISR(rx_queue, &character, NULL);
	}
}

/**
 * @brief Create the RX queue and enable interrupt-driven reception.
 *
 * @details Requires a configured UART with an IRQ source. Allocates a
 *          128-byte queue, connects the handler, enables the source in the
 *          interrupt controller and finally sets the received-data-available
 *          interrupt in the UART. Failing to connect the interrupt leaves the
 *          queue allocated but RX unarmed; the UART interrupt stays disabled.
 *
 * @return 0 on success, -ENODEV when the UART or its IRQ is missing, -ENOMEM
 *         when the queue cannot be allocated, or -EINVAL when the interrupt
 *         cannot be connected.
 */
int sunxi_uart_start_rx(void)
{
	if (uart_base == 0U || uart_irq == 0U) {
		return -ENODEV;
	}
	rx_queue = xQueueCreate(128, sizeof(uint8_t));
	if (rx_queue == NULL) {
		return -ENOMEM;
	}
	if (cs_irq_connect(uart_irq, uart_isr, NULL) != 0) {
		return -EINVAL;
	}
	cs_irq_enable(uart_irq);
	writel(IER_ERBFI, uart_base + IER_DLH);
	return 0;
}
