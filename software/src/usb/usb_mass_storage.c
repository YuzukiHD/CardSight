/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The block device exported over USB: the MUSB controller in device mode, the SD card as one
 * mass storage LUN. The stack moves the bulk data through the reserved on-chip SRAM block; the
 * card reads and writes go through the block layer.
 */

#define LOG_MODULE "usb-msc"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <board/cardsight_board.h>
#include <hal/blkdev.h>
#include <hal/kernel.h>
#include <init.h>
#include <log.h>
#include <usb/mass_storage.h>

#include "usbd_core.h"
#include "usbd_msc.h"

#define MSC_IN_EP  0x81U
#define MSC_OUT_EP 0x01U
#define MSC_CONFIG_LENGTH (9U + MSC_DESCRIPTOR_LEN)

static const uint8_t device_descriptor[] = { USB_DEVICE_DESCRIPTOR_INIT(
	USB_2_0, 0x00, 0x00, 0x00, 0x1d6b, 0x0105, 0x0100, 0x01) };

#define MSC_CONFIG(max_packet)                                                                  \
	USB_CONFIG_DESCRIPTOR_INIT(MSC_CONFIG_LENGTH, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100), \
		MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, max_packet, 0x04)

static const uint8_t config_hs[] = { MSC_CONFIG(USB_BULK_EP_MPS_HS) };
static const uint8_t config_fs[] = { MSC_CONFIG(USB_BULK_EP_MPS_FS) };
static const uint8_t quality_descriptor[] = {
	USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01),
};
static const uint8_t other_hs[] = {
	USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(MSC_CONFIG_LENGTH, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100),
	MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, USB_BULK_EP_MPS_FS, 0x04),
};
static const uint8_t other_fs[] = {
	USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(MSC_CONFIG_LENGTH, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100),
	MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, USB_BULK_EP_MPS_HS, 0x04),
};
static const char *strings[] = {
	(const char[]){ 0x09, 0x04 },
	"CardSight",
	"CardSight SD Card",
	"CARD SIGHT MSC01",
	"Mass Storage",
};

static const uint8_t *descriptor_device(uint8_t speed)
{
	(void)speed;
	return device_descriptor;
}

static const uint8_t *descriptor_config(uint8_t speed)
{
	return speed == USB_SPEED_HIGH ? config_hs : config_fs;
}

static const uint8_t *descriptor_quality(uint8_t speed)
{
	(void)speed;
	return quality_descriptor;
}

static const uint8_t *descriptor_other(uint8_t speed)
{
	return speed == USB_SPEED_HIGH ? other_hs : other_fs;
}

static const char *descriptor_string(uint8_t speed, uint8_t index)
{
	(void)speed;
	return index < ARRAY_SIZE(strings) ? strings[index] : NULL;
}

/* The device stack takes the transfer buffer from here: the reserved SRAM block. */
uint8_t *usbd_msc_get_buffer(uint8_t busid)
{
	return busid == 0U ? (uint8_t *)SUNXI_MSC_BUFFER_ADDRESS : NULL;
}

static const struct usb_descriptor descriptors = {
	.device_descriptor_callback = descriptor_device,
	.config_descriptor_callback = descriptor_config,
	.device_quality_descriptor_callback = descriptor_quality,
	.other_speed_descriptor_callback = descriptor_other,
	.string_descriptor_callback = descriptor_string,
};

static struct {
	struct block_device *bdev;
	struct usbd_interface interface;
	struct mutex lock;
	volatile bool started;
	volatile bool configured;
	volatile int error;
	bool read_only;
	bool force_read_only;
	uint64_t bytes_read;
	uint64_t bytes_written;
} storage;

void usb_mass_storage_force_read_only(bool read_only)
{
	storage.force_read_only = read_only;
}

static bool disk_mounted(const struct gendisk *disk)
{
	const struct block_device *part;

	if (disk->part0->mounts != 0U) {
		return true;
	}
	for (part = disk->parts; part != NULL; part = part->next) {
		if (part->mounts != 0U) {
			return true;
		}
	}
	return false;
}

static void usb_event(uint8_t busid, uint8_t event)
{
	(void)busid;
	if (event == USBD_EVENT_CONFIGURED) {
		storage.configured = true;
	} else if (event == USBD_EVENT_RESET || event == USBD_EVENT_DISCONNECTED || event == USBD_EVENT_DEINIT) {
		storage.configured = false;
	}
}

/* ---- what the mass storage class asks of the device ---- */

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_count, uint32_t *block_size)
{
	if (busid != 0U || lun != 0U || storage.bdev == NULL) {
		*block_count = 0U;
		*block_size = 512U;
		return;
	}
	*block_count = (uint32_t)bdev_nr_sectors(storage.bdev);
	*block_size = bdev_sector_size(storage.bdev);
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length)
{
	uint32_t sector_size;
	int result;

	if (busid != 0U || lun != 0U || storage.bdev == NULL) {
		return -ENODEV;
	}
	sector_size = bdev_sector_size(storage.bdev);
	if (length == 0U || length % sector_size != 0U) {
		return -EINVAL;
	}
	result = bdev_read(storage.bdev, sector, buffer, length / sector_size);
	if (result != 0) {
		storage.error = result;
		return result;
	}
	storage.bytes_read += length;
	return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer, uint32_t length)
{
	uint32_t sector_size;
	int result;

	if (busid != 0U || lun != 0U || storage.bdev == NULL) {
		return -ENODEV;
	}
	if (storage.read_only) {
		return -EROFS;
	}
	sector_size = bdev_sector_size(storage.bdev);
	if (length == 0U || length % sector_size != 0U) {
		return -EINVAL;
	}
	result = bdev_write(storage.bdev, sector, buffer, length / sector_size);
	if (result != 0) {
		storage.error = result;
		return result;
	}
	storage.bytes_written += length;
	return 0;
}

/* ---- start and stop ---- */

int usb_mass_storage_start_device(const char *device_name)
{
	struct block_device *bdev;
	int result = 0;

	mutex_lock_timeout(&storage.lock, WAIT_FOREVER);
	if (storage.started) {
		result = -EBUSY;
		goto out;
	}
	if (device_name == NULL || device_name[0] == '\0') {
		result = -EINVAL;
		goto out;
	}
	bdev = bdev_open_by_name(device_name);
	if (bdev == NULL) {
		result = -ENODEV;
		goto out;
	}
	if (bdev_sector_size(bdev) != 512U || bdev_nr_sectors(bdev) == 0U ||
	    bdev_nr_sectors(bdev) > UINT32_MAX || disk_mounted(bdev->bd_disk)) {
		result = -EINVAL;
		goto close;
	}
	storage.bdev = bdev;
	storage.read_only = storage.force_read_only || bdev_read_only(bdev);
	storage.configured = false;
	storage.error = 0;
	storage.bytes_read = 0U;
	storage.bytes_written = 0U;
	usbd_desc_register(0U, &descriptors);
	if (usbd_msc_init_intf(0U, &storage.interface, MSC_OUT_EP, MSC_IN_EP) == NULL) {
		storage.bdev = NULL;
		result = -ENOMEM;
		goto close;
	}
	usbd_add_interface(0U, &storage.interface);
	usbd_msc_set_readonly(0U, storage.read_only);
	if (usbd_initialize(0U, 0x04100000UL, usb_event) != 0) {
		usbd_deinitialize(0U);
		storage.bdev = NULL;
		result = -EIO;
		goto close;
	}
	storage.started = true;
	pr_info("USB mass storage: %s, %llu sectors, %s\n", bdev_name(bdev),
		(unsigned long long)bdev_nr_sectors(bdev), storage.read_only ? "read-only" : "read-write");
	goto out;
close:
	bdev_close(bdev);
out:
	if (result != 0) {
		storage.error = result;
	}
	mutex_unlock(&storage.lock);
	return result;
}

void usb_mass_storage_stop(void)
{
	mutex_lock_timeout(&storage.lock, WAIT_FOREVER);
	if (storage.started) {
		usbd_deinitialize(0U);
		(void)bdev_flush(storage.bdev);
		bdev_close(storage.bdev);
		storage.bdev = NULL;
		storage.started = false;
		storage.configured = false;
	}
	mutex_unlock(&storage.lock);
}

bool usb_mass_storage_running(void)
{
	return storage.started;
}

bool usb_mass_storage_configured(void)
{
	return storage.configured;
}

int usb_mass_storage_last_error(void)
{
	return storage.error;
}

void usb_mass_storage_get_stats(struct usb_mass_storage_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	stats->bytes_read = storage.bytes_read;
	stats->bytes_written = storage.bytes_written;
}

static int usb_mass_storage_init(void)
{
	mutex_init(&storage.lock);
	return 0;
}
INITCALL(1, 30, usb_mass_storage_init);
