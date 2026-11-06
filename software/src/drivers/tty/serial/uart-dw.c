/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The DesignWare APB UART (8250 compatible, 4-byte register stride) as the console.
 *
 * TX is polled so it works from the first instruction; RX is interrupt driven once the scheduler
 * runs and feeds a queue the shell reads.
 */

#include <hal/io.h>
#include <hal/kernel.h>

#include "drivers/drivers.h"
#include "arch/arch.h"

#define RBR_THR_DLL 0x00U
#define IER_DLH	    0x04U
#define IIR_FCR	    0x08U
#define LCR	    0x0cU
#define MCR	    0x10U
#define LSR	    0x14U
#define USR	    0x7cU

#define LSR_DR	 0x01U
#define LSR_THRE 0x20U
#define IER_ERBFI 0x01U

static uintptr_t uart_base;
static uint32_t uart_irq;
static QueueHandle_t rx_queue;

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

void sunxi_uart_putc(char character)
{
	if (uart_base == 0U) {
		return;
	}
	while ((readl(uart_base + LSR) & LSR_THRE) == 0U) {
	}
	writel((uint8_t)character, uart_base + RBR_THR_DLL);
}

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
