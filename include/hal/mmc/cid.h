/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The card identification register, decoded.
 */

#ifndef CARDSIGHT_MMC_CID_H
#define CARDSIGHT_MMC_CID_H

#include <stdint.h>

struct mmc_cid_identity {
	uint8_t manufacturer_id;
	char oem_id[3];
	char product_name[6];
	uint8_t product_revision_major;
	uint8_t product_revision_minor;
	uint32_t serial_number;
	uint16_t manufacture_year;
	uint8_t manufacture_month;
};

void mmc_decode_cid(const uint8_t cid[16], struct mmc_cid_identity *identity);

#endif /* CARDSIGHT_MMC_CID_H */
