/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file usb_config.h
 * @brief CherryUSB configuration for this board: the MUSB device controller and mass storage.
 *
 * The class runs its transfers in a thread, and the transfer buffer is the reserved on-chip
 * SRAM block. Caching is enabled, with 64-byte alignment for the buffers the controller sees.
 */

#ifndef CHERRYUSB_CONFIG_H
#define CHERRYUSB_CONFIG_H

/* the stack logs on the console; only its errors are interesting on a working board */
void usb_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define CONFIG_USB_PRINTF(...) usb_printf(__VA_ARGS__)
#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR
#endif

/* the device stack passes cache line aligned buffers to the controller */
#define CONFIG_USB_DCACHE_ENABLE
#define CONFIG_USB_ALIGN_SIZE 64
#define USB_NOCACHE_RAM_SECTION

#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#define CONFIG_USBDEV_ADVANCE_DESC
#define CONFIG_USBDEV_EP0_THREAD
#define CONFIG_USBDEV_EP0_PRIO 4
#define CONFIG_USBDEV_EP0_STACKSIZE 2048

#define CONFIG_USBDEV_MSC_THREAD
#define CONFIG_USBDEV_MSC_PRIO 4
#define CONFIG_USBDEV_MSC_STACKSIZE 8192
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 65536
#define CONFIG_USBDEV_MSC_EXTERNAL_BUFFER
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING "CardSight"
#define CONFIG_USBDEV_MSC_PRODUCT_STRING "SD Card"
#define CONFIG_USBDEV_MSC_VERSION_STRING "1.00"

#define CONFIG_USB_HS
#define CONFIG_USB_MUSB_EP_NUM 4
#define CONFIG_USB_MUSB_PIPE_NUM 4
#define CONFIG_USB_MUSB_SUNXI

#endif /* CHERRYUSB_CONFIG_H */
