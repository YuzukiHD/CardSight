/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file mass_storage.h
 * @brief A block device exported as a USB mass storage LUN: the host sees a disk and reads and
 * writes the sectors of the block device behind it.
 */

#ifndef CARDSIGHT_USB_MASS_STORAGE_H
#define CARDSIGHT_USB_MASS_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Transfer counters of the exported disk.
 */
struct usb_mass_storage_stats {
	uint64_t bytes_read; /**< Bytes the host has read. */
	uint64_t bytes_written; /**< Bytes the host has written. */
};

/**
 * @brief Export read-only whatever the device says.
 *
 * For host-side read tests.
 *
 * @param[in] read_only Whether the export refuses host writes.
 */
void usb_mass_storage_force_read_only(bool read_only);

/**
 * @brief Start the device stack and export a block device.
 *
 * @param[in] device_name Block device to export, e.g. "mmcblk0".
 * @return 0 on success, -EBUSY while it already runs, -EINVAL for an empty name, or -ENODEV
 * when the block device does not exist.
 */
int usb_mass_storage_start_device(const char *device_name);
/** @brief Stop the device stack and release the exported block device. */
void usb_mass_storage_stop(void);

/**
 * @brief Whether the stack runs and a disk is exported.
 *
 * @return true while a disk is exported.
 */
bool usb_mass_storage_running(void);
/**
 * @brief Whether the host configured the interface (it enumerated us).
 *
 * @return true after the host configured the interface.
 */
bool usb_mass_storage_configured(void);
/**
 * @brief The last error recorded by the mass storage stack.
 *
 * @return The error, or 0 when there was none.
 */
int usb_mass_storage_last_error(void);
/**
 * @brief Copy the transfer counters out.
 *
 * @param[out] stats Receives the byte counts; NULL is ignored.
 */
void usb_mass_storage_get_stats(struct usb_mass_storage_stats *stats);

#endif /* CARDSIGHT_USB_MASS_STORAGE_H */
