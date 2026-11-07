/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file cid.h
 * @brief The card identification register, decoded.
 */

#ifndef CARDSIGHT_MMC_CID_H
#define CARDSIGHT_MMC_CID_H

#include <stdint.h>

/**
 * @brief Friendly fields decoded from the card identification register.
 */
struct mmc_cid_identity {
	uint8_t manufacturer_id; /**< Manufacturer ID byte. */
	char oem_id[3]; /**< OEM/application ID, NUL terminated. */
	char product_name[6]; /**< Product name, NUL terminated. */
	uint8_t product_revision_major; /**< Major product revision. */
	uint8_t product_revision_minor; /**< Minor product revision. */
	uint32_t serial_number; /**< Card serial number. */
	uint16_t manufacture_year; /**< Year of manufacture. */
	uint8_t manufacture_month; /**< Month of manufacture, 1 to 12. */
};

/**
 * @brief Decode a raw CID register into identity fields.
 *
 * @param[in]  cid      The 16-byte card identification register.
 * @param[out] identity Receives the decoded fields.
 */
void mmc_decode_cid(const uint8_t cid[16], struct mmc_cid_identity *identity);

#endif /* CARDSIGHT_MMC_CID_H */
