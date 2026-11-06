/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The tiny device model the mmc card and the block layer need: a name and a reference count.
 * There is no bus, driver or class matching here; the platform knows its hardware.
 */

#ifndef CARDSIGHT_DEVICE_H
#define CARDSIGHT_DEVICE_H

#include <stdbool.h>

#define DEVICE_NAME_MAX 32

struct device {
	char name[DEVICE_NAME_MAX];
	struct device *parent;
	void (*release)(struct device *dev);
	int refcount;
};

void device_initialize(struct device *dev);
void get_device(struct device *dev);

/** Drop a reference; the release callback runs when the last one is gone. */
static inline void put_device(struct device *dev)
{
	if (dev != NULL && dev->refcount > 0 && --dev->refcount == 0 && dev->release != NULL) {
		dev->release(dev);
	}
}

#endif /* CARDSIGHT_DEVICE_H */
