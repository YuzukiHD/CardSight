/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A block device exported as a USB mass storage LUN: the host sees a disk and reads and writes
 * the sectors of the block device behind it.
 */

#ifndef CARDSIGHT_USB_MASS_STORAGE_H
#define CARDSIGHT_USB_MASS_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

struct usb_mass_storage_stats {
	uint64_t bytes_read;
	uint64_t bytes_written;
};

/** Export read-only whatever the device says; for host-side read tests. */
void usb_mass_storage_force_read_only(bool read_only);

/** Start the device stack and export @p device_name; -EBUSY while it runs. */
int usb_mass_storage_start_device(const char *device_name);
void usb_mass_storage_stop(void);

/** The stack runs (a disk is exported). */
bool usb_mass_storage_running(void);
/** The host configured the interface (it enumerated us). */
bool usb_mass_storage_configured(void);
int usb_mass_storage_last_error(void);
void usb_mass_storage_get_stats(struct usb_mass_storage_stats *stats);

#endif /* CARDSIGHT_USB_MASS_STORAGE_H */
