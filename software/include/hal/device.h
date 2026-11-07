/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file device.h
 * @brief The tiny device model the mmc card and the block layer need: a name and a reference
 * count.
 *
 * There is no bus, driver or class matching here; the platform knows its hardware.
 */

#ifndef CARDSIGHT_DEVICE_H
#define CARDSIGHT_DEVICE_H

#include <stdbool.h>

/** @brief Longest device name, including the terminating zero. */
#define DEVICE_NAME_MAX 32

/**
 * @brief Reference-counted object shared between the driver and its users.
 */
struct device {
	char name[DEVICE_NAME_MAX]; /**< Name the object is known by. */
	struct device *parent; /**< Parent object, when there is one. */
	void (*release)(struct device *dev); /**< Called when the last reference goes. */
	int refcount; /**< Number of live references. */
};

/**
 * @brief Prepare a device structure: zero it and take the initial reference.
 *
 * @param[out] dev Device to initialize; must point to writable storage.
 */
void device_initialize(struct device *dev);
/** @brief Take one more reference; NULL is ignored. */
void get_device(struct device *dev);

/**
 * @brief Drop a reference; the release callback runs when the last one is gone.
 *
 * @details Decrements the count only while it is positive, so an over-release cannot wrap, and
 * invokes @c release once the count reaches zero. NULL is ignored.
 *
 * @param[in] dev Device to release.
 */
static inline void put_device(struct device *dev)
{
	if (dev != NULL && dev->refcount > 0 && --dev->refcount == 0 && dev->release != NULL) {
		dev->release(dev);
	}
}

#endif /* CARDSIGHT_DEVICE_H */
