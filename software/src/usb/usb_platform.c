/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file usb_platform.c
 * @brief MUSB controller platform glue for the board.
 *
 * Covers the clocks, the PHY, the device role wiring, the interrupt, and the cache and log
 * functions the device stack expects from its platform.
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

/** @brief Base address of the MUSB controller. */
#define OTG_BASE   0x04100000UL
/** @brief The PHY glue registers next to the controller. */
#define OTG_GLUE   0x04100400UL /* the PHY glue next to the controller */
/** @brief Vendor register holding the FIFO access mode. */
#define OTG_VEND0  0x43U

/** @brief Glue interrupt and status control register. */
#define GLUE_ISCR     0x00U
/** @brief Glue PHY control register. */
#define GLUE_PHYCTRL  0x10U
/** @brief Glue PHY role select register. */
#define GLUE_PHYSEL   0x20U

/** @brief Enable the D+/D- pull-ups. */
#define ISCR_DPDM_PULLUP_EN  (1U << 16)
/** @brief Enable the ID pin pull-up. */
#define ISCR_ID_PULLUP_EN    (1U << 17)
/** @brief Force the ID comparator to report a B device. */
#define ISCR_FORCE_ID_HIGH   (3U << 14)
/** @brief Force the VBUS comparator to report valid VBUS. */
#define ISCR_FORCE_VBUS_HIGH (3U << 12)
/** @brief The edge-change status bits cleared before the role is forced. */
#define ISCR_CHANGE_FLAGS    ((1U << 6) | (1U << 5) | (1U << 4))
/** @brief PHY suspend; clearing it powers the PHY up. */
#define PHYCTRL_SIDDQ	      (1U << 3)
/** @brief Select the OTG PHY. */
#define PHYSEL_OTG	      (1U << 0)
/** @brief Put the controller's endpoint FIFOs in PIO mode. */
#define VEND0_PIO	      (1U << 0)

/* the device peripheral is CLIC 45 in the SoC tables, PLIC 29 here */
/** @brief Interrupt line of the OTG controller (PLIC 29 here). */
#define USB_OTG_IRQ 29U

#ifndef CONFIG_USB_MUSB_SUNXI
#error the CherryUSB MUSB port needs its sunxi glue
#endif

/**
 * @brief Controller interrupt entry point provided by the device stack.
 *
 * @param[in] busid Controller index.
 */
void USBD_IRQHandler(uint8_t busid);

/**
 * @brief Interrupt handler that forwards the controller interrupt to the stack.
 *
 * @param[in] argument Unused.
 */
static void usb_otg_isr(void *argument)
{
	(void)argument;
	USBD_IRQHandler(0U);
}

/**
 * @brief Bring up the controller's clocks, PHY and interrupt.
 *
 * @param[in] busid Controller index; only bus 0 exists.
 *
 * Enables the PHY and controller clocks (gate and reset) in the CCU, forces the glue into the
 * B-device role with VBUS valid, clears the pending role-change flags, enables the D+ pull-up,
 * takes the PHY out of suspend, selects the OTG PHY, puts the endpoint FIFOs into PIO mode, and
 * finally connects and enables the controller interrupt.
 */
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

/**
 * @brief Shut the controller's interrupt and PHY down.
 *
 * @param[in] busid Controller index; only bus 0 exists.
 *
 * Disables the interrupt line and returns the PHY to suspend. The clocks stay on, so a later
 * init call can bring the controller back without touching the CCU again.
 */
void usb_dc_low_level_deinit(uint8_t busid)
{
	(void)busid;
	cs_irq_disable(USB_OTG_IRQ);
	setbits32(OTG_GLUE + GLUE_PHYCTRL, PHYCTRL_SIDDQ);
}

/* ---- what the device stack asks of its platform ---- */

/**
 * @brief Clean a range of the data cache.
 *
 * @param[in] address Start address.
 * @param[in] length  Length in bytes.
 */
void usb_dcache_clean(uintptr_t address, size_t length)
{
	dcache_clean_range((const void *)address, length);
}

/**
 * @brief Invalidate a range of the data cache.
 *
 * @param[in] address Start address.
 * @param[in] length  Length in bytes.
 */
void usb_dcache_invalidate(uintptr_t address, size_t length)
{
	dcache_invalidate_range((const void *)address, length);
}

/**
 * @brief Clean and invalidate a range of the data cache.
 *
 * @param[in] address Start address.
 * @param[in] length  Length in bytes.
 */
void usb_dcache_flush(uintptr_t address, size_t length)
{
	dcache_clean_range((const void *)address, length);
	dcache_invalidate_range((const void *)address, length);
}

/**
 * @brief Formatted output for the device stack.
 *
 * @param[in] format printf-style format string.
 * @param[in] ...    Format arguments.
 *
 * Formats into a 160-byte stack buffer and writes the result to the console. A longer message is
 * truncated; output that does not fit is never written past the buffer.
 */
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
