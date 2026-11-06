/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The card as the application sees it: what it is, how it is accessed and what the host found.
 * The structure is owned by the mmc core; get a reference with mmc_host_card_get().
 */

#ifndef CARDSIGHT_MMC_CARD_H
#define CARDSIGHT_MMC_CARD_H

#include <stdbool.h>
#include <stdint.h>
#include <hal/device.h>
#include <hal/mmc/cid.h>

struct mmc_host;

struct mmc_card_info {
	uint32_t sectors; /* 512 byte sectors */
	uint32_t clock;	  /* Hz in use */
	int bus_width;
	bool high_capacity; /* SDHC/SDXC (block addressed) */
	uint8_t cid[16];
	struct mmc_cid_identity identity;
	uint16_t rca;
	/* what the card offers, read during identification */
	bool s18a;	     /* the card accepted 1.8 V signalling in ACMD41 */
	uint16_t sw_groups[6]; /* CMD6 support masks of function groups 1..6 */
	uint16_t max_current;  /* mA, CMD6 status */
	const char *mode;      /* speed mode in use: SDR104, SDR50, DDR50, SDR25, default */
	bool tuned;	       /* the sample point was found by tuning */
};

struct mmc_card {
	struct device dev; /* the reference released by put_device() */
	struct mmc_host *host;
	struct mmc_card_info info;
	bool ready;
};

#endif /* CARDSIGHT_MMC_CARD_H */
