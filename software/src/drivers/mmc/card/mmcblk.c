/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file mmcblk.c
 * @brief The card as a block device, with the card lifecycle.
 *
 * mmcblk0 and its partitions, the card lifecycle (insertion, removal and hotplug), the shell
 * command and the reference the application takes with mmc_host_card_get().
 */

/** @brief Log module name used by the pr_*() helpers. */
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

/**
 * @brief The slot as the application sees it.
 *
 * @details The header keeps the type opaque; only the index exists here.
 */
struct mmc_host {
	/** @brief Slot number; the board has only slot 0. */
	uint32_t index;
};

/**
 * @brief Serialises block transfers.
 *
 * The host's descriptor list and bounce buffer are shared, so two transfers must not overlap.
 */
static struct mutex mmc_lock;
/** @brief Whether @c mmc_lock was initialised; transfers can start before the scheduler does. */
static bool mmc_lock_ready;

/** @brief The board's only slot. */
static struct mmc_host host0 = { .index = 0U };
/** @brief State of the card in the slot. */
static struct mmc_card card;
/** @brief Whether @c card is identified and published as mmcblk0. */
static bool card_ready;
/** @brief The published disk, or NULL when no card is present. */
static struct gendisk *card_disk;

/**
 * @brief When set, the hotplug code believes the slot is empty whatever the pin reads.
 *
 * The soak test drives the slot's state by hand; the pin itself is read normally.
 */
static bool card_absent_forced;

/**
 * @brief Take the transfer lock when it is ready.
 *
 * @details A transfer can be issued by early init before mmc_blk_init() has initialised the
 * mutex, so the lock is skipped until it exists.
 */
static void mmc_lock_acquire(void)
{
	if (mmc_lock_ready) {
		mutex_lock_timeout(&mmc_lock, WAIT_FOREVER);
	}
}

/**
 * @brief Release the transfer lock when it was taken.
 */
static void mmc_lock_release(void)
{
	if (mmc_lock_ready) {
		mutex_unlock(&mmc_lock);
	}
}

/**
 * @brief Report whether a card is in the slot, with debouncing.
 *
 * @details The switch in the slot can bounce, and a card that is being read can glitch it; a
 * change of state counts only when five readings 5 ms apart agree. At least three of the five
 * samples must see a card, and the forced-absence flag used by the soak test overrides the pin.
 *
 * @return true when a card is present.
 */
static bool card_present(void)
{
	uint32_t present = 0U;

	if (card_absent_forced) {
		return false;
	}
	for (uint32_t sample = 0U; sample < 5U; sample++) {
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

/**
 * @brief Block read operation of mmcblk0.
 *
 * @details Refuses the request when no card is published or when it runs past the end of the
 * card, then holds the transfer lock across the SD read.
 *
 * @param[in]  disk   Disk the request targets, unused.
 * @param[in]  sector First sector.
 * @param[out] buffer Destination buffer.
 * @param[in]  count  Number of sectors.
 * @return 0 on success, -EINVAL for an out-of-range request, or the SD error.
 */
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

/**
 * @brief Block write operation of mmcblk0.
 *
 * @details Refuses the request when no card is published or when it runs past the end of the
 * card, then holds the transfer lock across the SD write.
 *
 * @param[in] disk   Disk the request targets, unused.
 * @param[in] sector First sector.
 * @param[in] buffer Source buffer.
 * @param[in] count  Number of sectors.
 * @return 0 on success, -EINVAL for an out-of-range request, or the SD error.
 */
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

/**
 * @brief Block flush operation of mmcblk0.
 *
 * @details Runs the SD status poll under the transfer lock, because it uses the same command
 * engine as a transfer.
 *
 * @param[in] disk Disk the request targets, unused.
 * @return 0 on success, or the SD error.
 */
static int mmcblk_flush(struct gendisk *disk)
{
	int result;

	(void)disk;
	mmc_lock_acquire();
	result = sd_flush(&card);
	mmc_lock_release();
	return result;
}

/**
 * @brief Release callback of the disk.
 *
 * @details The card state is owned by this file and not by the disk, so there is nothing to
 * free here.
 *
 * @param[in] disk Disk being freed, unused.
 */
static void mmcblk_free(struct gendisk *disk)
{
	(void)disk;
}

/** @brief Operations the block layer calls on mmcblk0. */
static const struct block_device_operations mmcblk_operations = {
	.read = mmcblk_read,
	.write = mmcblk_write,
	.flush = mmcblk_flush,
	.free_disk = mmcblk_free,
};

/** @brief The last identification result, 0 after a good probe. */
static int probe_error;
/** @brief Time of the last logged identification failure, for the rate limit. */
static uint64_t probe_log_ms;

/**
 * @brief Report the result of the last card identification.
 *
 * @return 0 when the last probe succeeded, or the error it returned.
 */
int mmc_probe_error(void)
{
	return probe_error;
}

/**
 * @brief Report whether the hotplug code is currently identifying a card.
 *
 * @details True while a card is present but not yet published; the UI uses it to show that
 * the slot is busy rather than empty.
 *
 * @return true when identification is in progress.
 */
bool mmc_slot_detecting(void)
{
	return !card_absent_forced && card_present() && !card_ready;
}

/**
 * @brief Allocate and publish mmcblk0 for the card just identified.
 *
 * @details Copies the card's sector count into the disk, fixes the block size at 512 bytes and
 * registers the disk, which also scans its partition table. A failure releases the disk and
 * leaves @c card_disk NULL.
 *
 * @return 0 on success, -ENOMEM without a disk, or -EIO when registration failed.
 */
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

/**
 * @brief Unregister mmcblk0 if it is published.
 *
 * @details A disk whose partitions are still mounted refuses the normal removal, so the force
 * variant is used: the media is gone and the open partitions must fail their transfers.
 */
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

/**
 * @brief Identify the card in the slot and publish it as mmcblk0.
 *
 * @details Runs the SD identification, then marks the card ready and attaches it to slot 0.
 * A card that never answers is retried with a backoff, so the log keeps one summary in ten
 * seconds instead of five error lines twice a second. On success the identity is printed and
 * the disk registered; the application sees mmcblk0 appear.
 *
 * @return 0 on success, or the identification or registration error.
 */
static int mmc_probe_card(void)
{
	int result = sd_identify(&card);

	if (result != 0) {
		uint64_t now = ktime_get_ms();

		/* A card that never answers is retried with a backoff; keep one summary in ten
		 * seconds instead of five error lines twice a second. */
		if (result != probe_error || now - probe_log_ms >= 10000U) {
			pr_err("card identification failed: %d (%s)\n", result, mmc_last_error());
			probe_log_ms = now;
		}
		probe_error = result;
		return result;
	}
	probe_error = 0;
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
			capacity, card.info.mode, (uint32_t)(card.info.clock / 1000000U),
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

/**
 * @brief Retire the card that left the slot.
 *
 * @details Runs the state change under the transfer lock, so a transfer in flight finishes
 * before mmcblk0 sees anything, then unregisters the disk and its partitions.
 */
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

/**
 * @brief Watch the slot and drive insertion and removal.
 *
 * @details Samples the debounced presence and retries identification with a 500 ms period that
 * doubles up to 5 s while a card fails to answer, so a wedged card is not hammered. A card that
 * disappeared stops the disk and resets the backoff. A successful probe also resets it. The
 * loop never ends. The application sees mmcblk0 appear and disappear and stops the USB device
 * and the storage proxy in response.
 *
 * @param[in] argument Task argument, unused.
 */
static void mmc_hotplug_task(void *argument)
{
	uint32_t retry_ms = 500U;

	(void)argument;
	/* a card already in the slot at start-up is identified right away */
	if (card_present() && !card_ready) {
		(void)mmc_probe_card();
	}
	for (;;) {
		bool now = card_present();

		if (now && !card_ready) {
			if (mmc_probe_card() == 0) {
				retry_ms = 500U;
			} else {
				retry_ms = MIN(retry_ms * 2U, 5000U);
			}
		} else {
			if (!now) {
				probe_error = 0;
				mmc_remove_card();
			}
			retry_ms = 500U;
		}
		msleep(retry_ms);
	}
}

/**
 * @brief Start the hotplug task.
 *
 * @return 0 on success, or -ENOMEM when the task cannot be created.
 */
static int mmc_hotplug_start(void)
{
	return xTaskCreate(mmc_hotplug_task, "mmc-hotplug", 3072U, NULL, 1U, NULL) == pdPASS ? 0 :
											     -ENOMEM;
}
INITCALL(3, 50, mmc_hotplug_start);

/**
 * @brief Initialise the transfer lock and try the card already in the slot.
 *
 * @details Waits the board's power-up delay first, because the card needs a stable supply
 * before it answers. An empty slot is not an error: the hotplug task will handle the card when
 * it arrives.
 *
 * @return 0 on success, or the identification error.
 */
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

/**
 * @brief Look up one of the board's slots.
 *
 * @param[in] index Slot number.
 * @return The slot handle, or NULL when @p index is not 0.
 */
struct mmc_host *mmc_host_get(uint32_t index)
{
	return index == 0U ? &host0 : NULL;
}

/**
 * @brief Take a reference on the card in a slot.
 *
 * @details The reference keeps the card record alive while the caller reads from it; the card
 * itself can still be removed, in which case the caller's transfers fail.
 *
 * @param[in] host Slot handle from mmc_host_get().
 * @return The card, or NULL for an unknown slot or an empty one.
 */
struct mmc_card *mmc_host_card_get(struct mmc_host *host)
{
	if (host != &host0 || !card_ready) {
		return NULL;
	}
	get_device(&card.dev);
	return &card;
}

/**
 * @brief Report whether a card handle still refers to the published card.
 *
 * @param[in] target Card handle to check.
 * @return true while @p target is the slot's card and it is ready.
 */
bool mmc_card_ready(const struct mmc_card *target)
{
	return target != NULL && target == &card && card_ready;
}

/**
 * @brief Copy the identity of the card.
 *
 * @param[in]  target Card handle.
 * @param[out] info   Structure receiving the identity.
 * @return 0 on success, or -ENODEV when the handle no longer refers to a ready card.
 */
int mmc_card_info(const struct mmc_card *target, struct mmc_card_info *info)
{
	if (target != &card || !card_ready) {
		return -ENODEV;
	}
	*info = card.info;
	return 0;
}

#ifdef CONFIG_SHELL
/**
 * @brief Shell command: show the card and drive the hotplug path by hand.
 *
 * @details Without an argument it prints the card identity, the timing in use and the DMA
 * counters. "eject" and "insert" flip the forced-absence flag and wait for the hotplug task to
 * notice, which exercises the same path as the pin without touching the slot. "soak" repeats
 * that cycle and counts the rounds where the expected transition did not happen.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL for a zero soak count, -EBUSY when an eject did not take,
 *         -ENODEV when an insert did not identify a card, or -EIO when the soak saw failures.
 */
static int command_mmc(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "eject") == 0) {
		/* drive the hotplug path by hand: the slot stays as it is */
		card_absent_forced = true;
		for (uint32_t waited = 0U; waited < 3000U && card_ready; waited += 50U) {
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
		for (uint32_t waited = 0U; waited < 10000U && !card_ready; waited += 50U) {
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
		uint32_t cycles = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 20U;
		uint32_t failures = 0U;

		if (cycles == 0U) {
			return -EINVAL;
		}
		for (uint32_t cycle = 0U; cycle < cycles; cycle++) {
			/* remove, wait for the disk to go, then insert again */
			card_absent_forced = true;
			for (uint32_t waited = 0U; waited < 4000U && card_ready; waited += 50U) {
				msleep(50U);
			}
			if (card_ready) {
				failures++;
			}
			card_absent_forced = false;
			for (uint32_t waited = 0U; waited < 10000U && !card_ready; waited += 50U) {
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
			(uint32_t)(card.info.clock / 1000000U), card.info.bus_width,
			card.info.tuned ? " tuned" : "",
			card.info.high_capacity ? "SDHC/SDXC" : "SDSC");
	}
	{
		uint32_t direct;
		uint32_t bounce;

		sunxi_mmc_dma_stats(&direct, &bounce);
		printf("%u direct, %u bounce DMA transfers\n", (uint32_t)direct, (uint32_t)bounce);
	}
	return 0;
}
SHELL_CMD(mmc, "show the SD card; eject/insert/soak drive the hotplug path", command_mmc);
#endif
