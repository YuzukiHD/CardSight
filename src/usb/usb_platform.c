/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The MUSB controller of the board: clocks, PHY, the device role wiring, the interrupt, and the
 * cache and log functions the device stack expects from its platform.
 */

#define LOG_MODULE "usb"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <console.h>
#include <hal/cache.h>
#include <hal/io.h>
#include <hal/kernel.h>
#include <log.h>

#include "arch/arch.h"
#include "board/cardsight_board.h"
#include "drivers/drivers.h"
#include "usb_config.h"

#define OTG_BASE   0x04100000UL
#define OTG_GLUE   0x04100400UL /* the PHY glue next to the controller */
#define OTG_VEND0  0x43U

#define GLUE_ISCR     0x00U
#define GLUE_PHYCTRL  0x10U
#define GLUE_PHYSEL   0x20U

#define ISCR_DPDM_PULLUP_EN  (1U << 16)
#define ISCR_ID_PULLUP_EN    (1U << 17)
#define ISCR_FORCE_ID_HIGH   (3U << 14)
#define ISCR_FORCE_VBUS_HIGH (3U << 12)
#define ISCR_CHANGE_FLAGS    ((1U << 6) | (1U << 5) | (1U << 4))
#define PHYCTRL_SIDDQ	      (1U << 3)
#define PHYSEL_OTG	      (1U << 0)
#define VEND0_PIO	      (1U << 0)

/* the device peripheral is CLIC 45 in the SoC tables, PLIC 29 here */
#define USB_OTG_IRQ 29U

#ifndef CONFIG_USB_MUSB_SUNXI
#error the CherryUSB MUSB port needs its sunxi glue
#endif

void USBD_IRQHandler(uint8_t busid);

static void usb_otg_isr(void *argument)
{
	(void)argument;
	USBD_IRQHandler(0U);
}

void usb_dc_low_level_init(uint8_t busid)
{
	(void)busid;
	/* the PHY block and the controller: gates on, resets released */
	sunxi_ccu_enable(SUNXI_CCU_BASE + 0x0a70U, 31U, SUNXI_CCU_BASE + 0x0a70U, 30U);
	sunxi_ccu_enable(SUNXI_CCU_BASE + 0x0a8cU, 8U, SUNXI_CCU_BASE + 0x0a8cU, 24U);

	/* device role without the ID and VBUS comparators: a B device with VBUS valid */
	writel((readl(OTG_GLUE + GLUE_ISCR) & ~ISCR_CHANGE_FLAGS) | ISCR_DPDM_PULLUP_EN |
			ISCR_ID_PULLUP_EN | ISCR_FORCE_ID_HIGH | ISCR_FORCE_VBUS_HIGH,
		OTG_GLUE + GLUE_ISCR);
	clrbits32(OTG_GLUE + GLUE_PHYCTRL, PHYCTRL_SIDDQ);
	setbits32(OTG_GLUE + GLUE_PHYSEL, PHYSEL_OTG);
	/* the controller's endpoint FIFOs are PIO accesses */
	writeb(VEND0_PIO, OTG_BASE + OTG_VEND0);

	(void)cs_irq_connect(USB_OTG_IRQ, usb_otg_isr, NULL);
	cs_irq_enable(USB_OTG_IRQ);
}

void usb_dc_low_level_deinit(uint8_t busid)
{
	(void)busid;
	cs_irq_disable(USB_OTG_IRQ);
	setbits32(OTG_GLUE + GLUE_PHYCTRL, PHYCTRL_SIDDQ);
}

/* ---- what the device stack asks of its platform ---- */

void usb_dcache_clean(uintptr_t address, size_t length)
{
	dcache_clean_range((const void *)address, length);
}

void usb_dcache_invalidate(uintptr_t address, size_t length)
{
	dcache_invalidate_range((const void *)address, length);
}

void usb_dcache_flush(uintptr_t address, size_t length)
{
	dcache_clean_range((const void *)address, length);
	dcache_invalidate_range((const void *)address, length);
}

void usb_printf(const char *format, ...)
{
	char message[160];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	if (length > 0) {
		console_write(message, (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1U);
	}
}
