/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file usb_mass_storage.c
 * @brief The block device exported over USB as one mass storage LUN.
 *
 * The MUSB controller runs in device mode and the SD card is the only LUN. The stack moves the
 * bulk data through the reserved on-chip SRAM block; the card reads and writes go through the
 * block layer.
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

/** @brief Bulk IN endpoint address used by the mass storage interface. */
#define MSC_IN_EP  0x81U
/** @brief Bulk OUT endpoint address used by the mass storage interface. */
#define MSC_OUT_EP 0x01U
/** @brief Total length of the configuration descriptor: header plus MSC descriptor. */
#define MSC_CONFIG_LENGTH (9U + MSC_DESCRIPTOR_LEN)

/** @brief Device descriptor: USB 2.0, vendor 0x1d6b, product 0x0105, one configuration. */
static const uint8_t device_descriptor[] = { USB_DEVICE_DESCRIPTOR_INIT(
	USB_2_0, 0x00, 0x00, 0x00, 0x1d6b, 0x0105, 0x0100, 0x01) };

/**
 * @brief Build one speed's configuration descriptor.
 *
 * @param max_packet Bulk endpoint maximum packet size for the speed.
 */
#define MSC_CONFIG(max_packet)                                                                  \
	USB_CONFIG_DESCRIPTOR_INIT(MSC_CONFIG_LENGTH, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100), \
		MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, max_packet, 0x04)

/** @brief High-speed configuration descriptor. */
static const uint8_t config_hs[] = { MSC_CONFIG(USB_BULK_EP_MPS_HS) };
/** @brief Full-speed configuration descriptor. */
static const uint8_t config_fs[] = { MSC_CONFIG(USB_BULK_EP_MPS_FS) };
/** @brief Device qualifier descriptor for the high-speed capable device. */
static const uint8_t quality_descriptor[] = {
	USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01),
};
/** @brief Other-speed configuration used while running at high speed. */
static const uint8_t other_hs[] = {
	USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(MSC_CONFIG_LENGTH, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100),
	MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, USB_BULK_EP_MPS_FS, 0x04),
};
/** @brief Other-speed configuration used while running at full speed. */
static const uint8_t other_fs[] = {
	USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(MSC_CONFIG_LENGTH, 0x01, 0x01, USB_CONFIG_BUS_POWERED, 100),
	MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, USB_BULK_EP_MPS_HS, 0x04),
};
/** @brief String descriptors indexed by the USB string index. */
static const char *strings[] = {
	(const char[]){ 0x09, 0x04 },
	"CardSight",
	"CardSight SD Card",
	"CARD SIGHT MSC01",
	"Mass Storage",
};

/**
 * @brief Return the device descriptor.
 *
 * @param[in] speed Negotiated speed; unused because the descriptor is speed independent.
 * @return The device descriptor.
 */
static const uint8_t *descriptor_device(uint8_t speed)
{
	(void)speed;
	return device_descriptor;
}

/**
 * @brief Return the configuration descriptor for a speed.
 *
 * @param[in] speed Negotiated USB speed.
 * @return The high-speed or the full-speed configuration.
 */
static const uint8_t *descriptor_config(uint8_t speed)
{
	return speed == USB_SPEED_HIGH ? config_hs : config_fs;
}

/**
 * @brief Return the device qualifier descriptor.
 *
 * @param[in] speed Negotiated speed; unused.
 * @return The qualifier descriptor.
 */
static const uint8_t *descriptor_quality(uint8_t speed)
{
	(void)speed;
	return quality_descriptor;
}

/**
 * @brief Return the other-speed configuration descriptor for a speed.
 *
 * @param[in] speed Negotiated USB speed.
 * @return The descriptor describing the opposite speed.
 */
static const uint8_t *descriptor_other(uint8_t speed)
{
	return speed == USB_SPEED_HIGH ? other_hs : other_fs;
}

/**
 * @brief Return a string descriptor by index.
 *
 * @param[in] speed Negotiated speed; unused.
 * @param[in] index String index from the device descriptor.
 * @return The string, or NULL for an unknown index.
 */
static const char *descriptor_string(uint8_t speed, uint8_t index)
{
	(void)speed;
	return index < ARRAY_SIZE(strings) ? strings[index] : NULL;
}

/**
 * @brief Give the device stack its transfer buffer.
 *
 * @param[in] busid Controller index; only bus 0 exists.
 * @return The reserved on-chip SRAM block, or NULL for an unknown bus.
 *
 * The device stack takes the transfer buffer from here: the reserved SRAM block.
 */
uint8_t *usbd_msc_get_buffer(uint8_t busid)
{
	return busid == 0U ? (uint8_t *)SUNXI_MSC_BUFFER_ADDRESS : NULL;
}

/** @brief Descriptor callbacks handed to the device stack. */
static const struct usb_descriptor descriptors = {
	.device_descriptor_callback = descriptor_device,
	.config_descriptor_callback = descriptor_config,
	.device_quality_descriptor_callback = descriptor_quality,
	.other_speed_descriptor_callback = descriptor_other,
	.string_descriptor_callback = descriptor_string,
};

/**
 * @brief State shared by the class callbacks, the start/stop calls and the shell.
 *
 * The lock serialises start and stop; the class callbacks run from the device stack's own
 * threads. The read-only flag also follows usb_mass_storage_force_read_only().
 */
static struct {
	struct block_device *bdev;	/**< Exported device, or NULL when stopped. */
	struct usbd_interface interface;	/**< Interface handed to the device stack. */
	struct mutex lock;		/**< Serialises start and stop. */
	volatile bool started;		/**< An export has been started. */
	volatile bool configured;	/**< The host has configured the device. */
	volatile int error;		/**< Last error from start or a transfer. */
	bool read_only;			/**< The volume is exported read-only. */
	bool force_read_only;		/**< Read-only was requested regardless of the device. */
	uint64_t bytes_read;		/**< Bytes read by the host since the start. */
	uint64_t bytes_written;		/**< Bytes written by the host since the start. */
} storage;

/**
 * @brief Force the exported volume read-only regardless of the device itself.
 *
 * @param[in] read_only True to make the next start read-only.
 *
 * The setting is remembered until the next start; it does not change a running export.
 */
void usb_mass_storage_force_read_only(bool read_only)
{
	storage.force_read_only = read_only;
}

/**
 * @brief Check whether any partition of a disk is mounted.
 *
 * @param[in] disk Disk to inspect.
 * @return True when the whole disk or one of its partitions is mounted.
 *
 * Exporting a disk that the VFS has mounted would let the host and the firmware write the same
 * file system at once, so start refuses it.
 */
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

/**
 * @brief Device stack event callback.
 *
 * @param[in] busid Controller index; unused.
 * @param[in] event USBD_EVENT_* code.
 *
 * Only the configured state matters here: it becomes true on USBD_EVENT_CONFIGURED and false
 * again on reset, disconnect or deinit, which is what the UI polls.
 */
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

/**
 * @brief Report the geometry of one LUN to the mass storage class.
 *
 * @param[in]  busid       Controller index.
 * @param[in]  lun         Logical unit number.
 * @param[out] block_count Number of 512-byte blocks, or 0 when the LUN is not usable.
 * @param[out] block_size  Block size in bytes.
 */
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

/**
 * @brief Read a run of sectors for the mass storage class.
 *
 * @param[in]  busid   Controller index.
 * @param[in]  lun     Logical unit number.
 * @param[in]  sector  First sector.
 * @param[out] buffer  Destination buffer.
 * @param[in]  length  Transfer length in bytes, a whole number of sectors.
 * @return 0 on success, -ENODEV, -EINVAL, or the block layer's error.
 *
 * A failed transfer is recorded in @c storage.error so the shell can report why the host saw
 * an error. Successful bytes are added to the statistics.
 */
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

/**
 * @brief Write a run of sectors for the mass storage class.
 *
 * @param[in] busid   Controller index.
 * @param[in] lun     Logical unit number.
 * @param[in] sector  First sector.
 * @param[in] buffer  Source buffer.
 * @param[in] length  Transfer length in bytes, a whole number of sectors.
 * @return 0 on success, -ENODEV, -EROFS, -EINVAL, or the block layer's error.
 *
 * A read-only export refuses the write before touching the medium. As with reads, a failure is
 * recorded and successful bytes are counted.
 */
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

/**
 * @brief Start exporting a block device over USB.
 *
 * @param[in] device_name Block device to export.
 * @return 0 on success, -EBUSY when an export already runs, -EINVAL, -ENODEV, -ENOMEM, or
 *         -EIO when the controller cannot be initialised.
 *
 * The lock serialises start and stop. The device is opened and validated: 512-byte sectors,
 * a non-zero sector count that fits the SCSI 32-bit field, and no mounted partition. The state
 * is reset, the descriptors registered, the MSC interface created and added, and the
 * controller initialised at its base address. Any failure after the device was opened closes
 * it again and is recorded in @c storage.error; the interface and controller are unwound as
 * far as they were set up.
 */
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
	/* the SCSI layer carries a 32-bit sector count, and a mounted disk is in use */
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
	/* register the descriptors and build the MSC interface */
	usbd_desc_register(0U, &descriptors);
	if (usbd_msc_init_intf(0U, &storage.interface, MSC_OUT_EP, MSC_IN_EP) == NULL) {
		storage.bdev = NULL;
		result = -ENOMEM;
		goto close;
	}
	usbd_add_interface(0U, &storage.interface);
	usbd_msc_set_readonly(0U, storage.read_only);
	/* the controller base is fixed by the SoC */
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

/**
 * @brief Stop the USB export.
 *
 * Does nothing when no export runs. Otherwise the controller is deinitialised, the device is
 * flushed and closed, and the running and configured flags are cleared. Takes the same lock as
 * the start path.
 */
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

/**
 * @brief Report whether an export has been started.
 *
 * @return True between a successful start and a stop.
 */
bool usb_mass_storage_running(void)
{
	return storage.started;
}

/**
 * @brief Report whether the host has configured the device.
 *
 * @return True while the USB device is in the configured state.
 */
bool usb_mass_storage_configured(void)
{
	return storage.configured;
}

/**
 * @brief Return the last error seen by the export.
 *
 * @return The recorded negative errno, or 0.
 */
int usb_mass_storage_last_error(void)
{
	return storage.error;
}

/**
 * @brief Copy the transfer statistics.
 *
 * @param[out] stats Structure receiving the byte counters; ignored when NULL.
 *
 * The counters are reset on every successful start.
 */
void usb_mass_storage_get_stats(struct usb_mass_storage_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	stats->bytes_read = storage.bytes_read;
	stats->bytes_written = storage.bytes_written;
}

/**
 * @brief Init call that prepares the state lock.
 *
 * @return Always 0.
 */
static int usb_mass_storage_init(void)
{
	mutex_init(&storage.lock);
	return 0;
}
INITCALL(1, 30, usb_mass_storage_init);
