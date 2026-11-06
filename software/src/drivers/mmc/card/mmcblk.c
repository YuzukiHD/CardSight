/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The card as a block device: mmcblk0 and its partitions, the card lifecycle (insertion,
 * removal and hotplug), the shell command and the reference the application takes with
 * mmc_host_card_get().
 */

#define LOG_MODULE "mmc"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <hal/blkdev.h>
#include <hal/kernel.h>
#include <hal/mmc/card.h>
#include <hal/mmc/core.h>
#include <hal/mmc/host.h>
#include <init.h>
#include <log.h>
#include <shell.h>
#include <size.h>

#include "drivers/mmc/host/sunxi-mmc.h"
#include "drivers/mmc/mmc-internal.h"

/* The slot as the application sees it; the header keeps the type opaque. */
struct mmc_host {
	unsigned int index;
};

/* Block transfers are serialised: the host's descriptor list and bounce buffer are shared. */
static struct mutex mmc_lock;
static bool mmc_lock_ready;

static struct mmc_host host0 = { .index = 0U };
static struct mmc_card card;
static bool card_ready;
static struct gendisk *card_disk;

/* the soak test drives the slot's state by hand; the pin itself is read normally */
static bool card_absent_forced;

static void mmc_lock_acquire(void)
{
	if (mmc_lock_ready) {
		mutex_lock_timeout(&mmc_lock, WAIT_FOREVER);
	}
}

static void mmc_lock_release(void)
{
	if (mmc_lock_ready) {
		mutex_unlock(&mmc_lock);
	}
}

/*
 * The switch in the slot can bounce, and a card that is being read can glitch it; a change of
 * state counts only when five readings 5 ms apart agree.
 */
static bool card_present(void)
{
	unsigned int present = 0U;

	if (card_absent_forced) {
		return false;
	}
	for (unsigned int sample = 0U; sample < 5U; sample++) {
		if (sunxi_mmc_present()) {
			present++;
		}
		if (sample + 1U < 5U) {
			msleep(5U);
		}
	}
	return present >= 3U;
}

/* ---- the block device ---- */

static int mmcblk_read(struct gendisk *disk, uint64_t sector, void *buffer, uint32_t count)
{
	int result;

	(void)disk;
	if (!card_ready || sector + count > card.info.sectors) {
		return -EINVAL;
	}
	mmc_lock_acquire();
	result = sd_read(&card, sector, buffer, count);
	mmc_lock_release();
	return result;
}

static int mmcblk_write(struct gendisk *disk, uint64_t sector, const void *buffer, uint32_t count)
{
	int result;

	(void)disk;
	if (!card_ready || sector + count > card.info.sectors) {
		return -EINVAL;
	}
	mmc_lock_acquire();
	result = sd_write(&card, sector, buffer, count);
	mmc_lock_release();
	return result;
}

static int mmcblk_flush(struct gendisk *disk)
{
	int result;

	(void)disk;
	mmc_lock_acquire();
	result = sd_flush(&card);
	mmc_lock_release();
	return result;
}

static void mmcblk_free(struct gendisk *disk)
{
	(void)disk;
}

static const struct block_device_operations mmcblk_operations = {
	.read = mmcblk_read,
	.write = mmcblk_write,
	.flush = mmcblk_flush,
	.free_disk = mmcblk_free,
};

static int mmc_add_disk(void)
{
	card_disk = alloc_disk();
	if (card_disk == NULL) {
		return -ENOMEM;
	}
	strcpy(card_disk->disk_name, "mmcblk0");
	card_disk->fops = &mmcblk_operations;
	card_disk->capacity = card.info.sectors;
	card_disk->logical_block_size = 512U;
	if (add_disk(card_disk) != 0) {
		put_disk(card_disk);
		card_disk = NULL;
		return -EIO;
	}
	return 0;
}

static void mmc_remove_disk(void)
{
	if (card_disk != NULL) {
		if (del_gendisk(card_disk) != 0) {
			del_gendisk_force(card_disk);
		}
		put_disk(card_disk);
		card_disk = NULL;
	}
}

/* Identify the card in the slot and publish it as mmcblk0; also runs on insertion. */
static int mmc_probe_card(void)
{
	int result = sd_identify(&card);

	if (result != 0) {
		pr_err("card identification failed: %d\n", result);
		return result;
	}
	card_ready = true;
	card.ready = true;
	card.host = &host0;
	if (card.dev.refcount == 0) {
		card.dev.refcount = 1;
	}
	card.dev.release = NULL;
	{
		char capacity[16];

		format_capacity((uint64_t)card.info.sectors * 512U, capacity, sizeof(capacity));
		pr_info("SD card: %s %s, %s, %u MHz, %u-bit%s\n", card.info.identity.product_name,
			capacity, card.info.mode, (unsigned)(card.info.clock / 1000000U),
			card.info.bus_width, card.info.tuned ? ", tuned" : "");
		result = mmc_add_disk();
		if (result != 0) {
			pr_err("cannot publish mmcblk0: %d\n", result);
			return result;
		}
		pr_info("mmcblk0: %s\n", capacity);
	}
	return 0;
}

/* The card went away: no new transfers, and the disk and its partitions go with it. */
static void mmc_remove_card(void)
{
	if (!card_ready) {
		return;
	}
	mmc_lock_acquire();
	card_ready = false;
	card.ready = false;
	mmc_lock_release();
	mmc_remove_disk();
	pr_info("SD card removed\n");
}

/*
 * Watch the slot: a card that is inserted is identified and published, a card that is taken
 * out stops the disk. The application sees mmcblk0 appear and disappear and stops the USB
 * device and the storage proxy in response.
 */
static void mmc_hotplug_task(void *argument)
{
	(void)argument;
	if (card_present() && !card_ready) {
		(void)mmc_probe_card();
	}
	for (;;) {
		bool now = card_present();

		if (now && !card_ready) {
			(void)mmc_probe_card();
		} else if (!now && card_ready) {
			mmc_remove_card();
		}
		msleep(500U);
	}
}

static int mmc_hotplug_start(void)
{
	return xTaskCreate(mmc_hotplug_task, "mmc-hotplug", 3072U, NULL, 1U, NULL) == pdPASS ? 0 :
											     -ENOMEM;
}
INITCALL(3, 50, mmc_hotplug_start);

int mmc_blk_init(void)
{
	mutex_init(&mmc_lock);
	mmc_lock_ready = true;
	msleep(sunxi_mmc0.power_delay_ms);
	if (!card_present()) {
		pr_info("no card in the slot\n");
		return 0;
	}
	return mmc_probe_card();
}
INITCALL(2, 45, mmc_blk_init);

/* ---- the application API ---- */

struct mmc_host *mmc_host_get(unsigned int index)
{
	return index == 0U ? &host0 : NULL;
}

struct mmc_card *mmc_host_card_get(struct mmc_host *host)
{
	if (host != &host0 || !card_ready) {
		return NULL;
	}
	get_device(&card.dev);
	return &card;
}

bool mmc_card_ready(const struct mmc_card *target)
{
	return target != NULL && target == &card && card_ready;
}

int mmc_card_info(const struct mmc_card *target, struct mmc_card_info *info)
{
	if (target != &card || !card_ready) {
		return -ENODEV;
	}
	*info = card.info;
	return 0;
}

#ifdef CONFIG_SHELL
static int command_mmc(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "eject") == 0) {
		/* drive the hotplug path by hand: the slot stays as it is */
		card_absent_forced = true;
		for (unsigned int waited = 0U; waited < 3000U && card_ready; waited += 50U) {
			msleep(50U);
		}
		if (card_ready) {
			printf("eject: the card is still published\n");
			return -EBUSY;
		}
		printf("eject: card removed\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "insert") == 0) {
		card_absent_forced = false;
		for (unsigned int waited = 0U; waited < 10000U && !card_ready; waited += 50U) {
			msleep(50U);
		}
		if (!card_ready) {
			printf("insert: no card identified\n");
			return -ENODEV;
		}
		printf("insert: card identified\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "soak") == 0) {
		unsigned int cycles = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 0) : 20U;
		unsigned int failures = 0U;

		if (cycles == 0U) {
			return -EINVAL;
		}
		for (unsigned int cycle = 0U; cycle < cycles; cycle++) {
			card_absent_forced = true;
			for (unsigned int waited = 0U; waited < 4000U && card_ready; waited += 50U) {
				msleep(50U);
			}
			if (card_ready) {
				failures++;
			}
			card_absent_forced = false;
			for (unsigned int waited = 0U; waited < 10000U && !card_ready; waited += 50U) {
				msleep(50U);
			}
			if (!card_ready) {
				failures++;
			}
		}
		printf("hotplug soak: %u cycles, %u failures\n", cycles, failures);
		return failures == 0U ? 0 : -EIO;
	}
	if (!card_ready) {
		printf("no card\n");
		return 0;
	}
	{
		char capacity[16];

		format_capacity((uint64_t)card.info.sectors * 512U, capacity, sizeof(capacity));
		printf("%.5s v%u.%u, %s, %s %u MHz %u-bit%s, %s\n", card.info.identity.product_name,
			card.info.identity.product_revision_major,
			card.info.identity.product_revision_minor, capacity, card.info.mode,
			(unsigned)(card.info.clock / 1000000U), card.info.bus_width,
			card.info.tuned ? " tuned" : "",
			card.info.high_capacity ? "SDHC/SDXC" : "SDSC");
	}
	{
		uint32_t direct;
		uint32_t bounce;

		sunxi_mmc_dma_stats(&direct, &bounce);
		printf("%u direct, %u bounce DMA transfers\n", (unsigned)direct, (unsigned)bounce);
	}
	return 0;
}
SHELL_CMD(mmc, "show the SD card; eject/insert/soak drive the hotplug path", command_mmc);
#endif
