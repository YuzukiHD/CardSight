/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file sd.c
 * @brief The SD protocol: identification, signalling switch, speed mode, tuning and transfers.
 *
 * Card identification, the 1.8 V signalling switch, speed mode selection and the CMD19 tuning
 * scan, plus block reads and writes. Everything here goes through the host calls in
 * mmc_internal.h; no register of the controller is touched from this file.
 */

/** @brief Log module name used by the pr_*() helpers. */
#define LOG_MODULE "mmc"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <hal/kernel.h>
#include <hal/mmc/card.h>
#include <log.h>

#include "drivers/mmc/mmc-internal.h"

/* ---- the commands this driver uses ---- */

/** @brief CMD0: reset the card to the idle state. */
#define SD_CMD_GO_IDLE		  0U
/** @brief CMD2: every card answers with its CID. */
#define SD_CMD_ALL_SEND_CID	  2U
/** @brief CMD3: publish a relative card address. */
#define SD_CMD_SEND_RCA		  3U
/** @brief CMD6: read or write one of the card's function switches. */
#define SD_CMD_SWITCH		  6U
/** @brief CMD7: select the card by its RCA. */
#define SD_CMD_SELECT_CARD	  7U
/** @brief CMD8: interface condition, tells an SD v2 card from an older one. */
#define SD_CMD_SEND_IF_COND	  8U
/** @brief CMD9: the selected card's CSD. */
#define SD_CMD_SEND_CSD		  9U
/** @brief CMD11: switch the card to 1.8 V signalling. */
#define SD_CMD_SWITCH_VOLTAGE	  11U
/** @brief CMD13: the card status, also a way to wait out a busy card. */
#define SD_CMD_SEND_STATUS	  13U
/** @brief CMD16: block length for a standard-capacity card. */
#define SD_CMD_SET_BLOCKLEN	  16U
/** @brief CMD17: read one block. */
#define SD_CMD_READ_SINGLE	  17U
/** @brief CMD18: read blocks until CMD12. */
#define SD_CMD_READ_MULTIPLE	  18U
/** @brief CMD19: read the tuning block at the current sample point. */
#define SD_CMD_SEND_TUNING_BLOCK  19U
/** @brief CMD24: write one block. */
#define SD_CMD_WRITE_SINGLE	  24U
/** @brief CMD25: write blocks until CMD12. */
#define SD_CMD_WRITE_MULTIPLE	  25U
/** @brief CMD55: the next command is an application command. */
#define SD_CMD_APP_CMD		  55U
/** @brief ACMD6: bus width selection. */
#define SD_ACMD_SET_BUS_WIDTH	  6U
/** @brief ACMD41: send the operating conditions until the card is ready. */
#define SD_ACMD_SEND_OP_COND	  41U

/** @brief Largest number of blocks given to one data command. */
#define SD_MAX_BLOCKS 256U /* per data command */

/* tuning: phases and sampling delays tried, repeats that must pass, window needed to accept */
/** @brief Number of sampling phases the scan tries. */
#define TUNE_PHASES	 3U
/** @brief Number of sampling delays tried in each phase. */
#define TUNE_DELAYS	 64U
/** @brief Reads that must pass in a row before a point counts as good. */
#define TUNE_REPEAT	 4U
/** @brief Reads that verify the chosen point before it is accepted. */
#define TUNE_VERIFY	 16U
/** @brief Shortest run of good delays that counts as a window. */
#define TUNE_MIN_WINDOW	 4U
/** @brief Size of the tuning block in bytes. */
#define TUNE_BLOCK_BYTES 64U

/** @brief Relative address the card published in CMD3; 0 before it is known. */
static uint32_t card_rca;
/** @brief Whether @c saved_tune_phase and @c saved_tune_delay hold a verified point. */
static bool saved_tune_valid;
/** @brief Sampling phase of the last good tuning point. */
static uint32_t saved_tune_phase;
/** @brief Sampling delay of the last good tuning point. */
static uint32_t saved_tune_delay;

/**
 * @brief Send a command with a response and no data phase.
 *
 * @param[in]  opcode        Command index.
 * @param[in]  argument      Command argument.
 * @param[in]  response_kind Expected response shape.
 * @param[out] response      Four words receiving the response.
 * @return 0 on success, or the host error.
 */
static int sd_command(uint32_t opcode, uint32_t argument, enum mmc_response response_kind,
	uint32_t response[4])
{
	return sunxi_mmc_command(opcode, argument, response_kind, response, NULL, 0U, 512U, false, 200U);
}

/**
 * @brief Send an application command: CMD55 first, then the command itself.
 *
 * @details The prefix carries the card's RCA in the argument's upper half word. A failed
 * prefix aborts, because the card would treat the next command as a regular one.
 *
 * @param[in]  opcode        Application command index.
 * @param[in]  argument      Command argument.
 * @param[in]  response_kind Expected response shape.
 * @param[out] response      Four words receiving the response.
 * @return 0 on success, or the error of the prefix or of the command.
 */
static int sd_app_command(uint32_t opcode, uint32_t argument, enum mmc_response response_kind,
	uint32_t response[4])
{
	uint32_t scratch[4];
	int result = sd_command(SD_CMD_APP_CMD, card_rca << 16, MMC_RESP_R1, scratch);

	return result != 0 ? result : sd_command(opcode, argument, response_kind, response);
}

/**
 * @brief Run a CMD6 switch and read back its status block.
 *
 * @details CMD6 returns its 512-bit status as data; the block is 64 bytes.
 *
 * @param[in]  argument Selects the group, the function and whether to write it.
 * @param[out] status   Receives the 64-byte status block.
 * @return 0 on success, or the host error.
 */
static int sd_switch(uint32_t argument, uint8_t status[64])
{
	uint32_t response[4];

	return sunxi_mmc_command(SD_CMD_SWITCH, argument, MMC_RESP_R1, response, status, 1U, 64U, false, 100U);
}

/**
 * @brief Perform the 1.8 V signalling switch.
 *
 * @details The 1.8 V signalling switch: CMD11 makes the card pull DAT0 low, the host stops the
 * clock, switches the supply of the card port and restarts the clock, and the card lets DAT0
 * go within a millisecond. A card that took the switch stays at 1.8 V until its power is
 * removed. Both transitions of DAT0 are checked, so a card that did not enter or did not leave
 * the handshake fails the sequence instead of being driven at the wrong level.
 *
 * @return 0 on success, -EIO when DAT0 did not follow the handshake, or the host error.
 */
static int sd_switch_to_1v8(void)
{
	uint32_t response[4];
	int result = sd_command(SD_CMD_SWITCH_VOLTAGE, 0U, MMC_RESP_R1, response);

	if (result != 0) {
		pr_err("CMD11 failed: %d\n", result);
		return result;
	}
	msleep(1U);
	/* the card acknowledges the switch by holding DAT0 low */
	if (!sunxi_mmc_card_busy()) {
		pr_err("CMD11: the card did not pull DAT0 low\n");
		return -EIO;
	}
	result = sunxi_mmc_voltage_switch(1800U);
	if (result != 0) {
		return result;
	}
	msleep(1U);
	/* and releases it once it runs at 1.8 V */
	if (sunxi_mmc_card_busy()) {
		pr_err("CMD11: the card holds DAT0 after the switch\n");
		return -EIO;
	}
	return 0;
}

/**
 * @brief Decode the CID response into the card identity.
 *
 * @details CID bytes 0..15 come from RESP3 down to RESP0, most significant byte first. The
 * product name is five characters, the revision two bytes, the serial number four bytes and
 * the manufacture date is packed as year-minus-2000 in the high nibble and the month in the
 * low nibble of the last byte.
 *
 * @param[in]  response Four response words from CMD2.
 * @param[out] info     Card info whose @c cid and @c identity fields are filled.
 */
static void sd_decode_cid(const uint32_t response[4], struct mmc_card_info *info)
{
	/* CID bytes 0..15 come from RESP3 down to RESP0, most significant byte first */
	for (uint32_t index = 0U; index < 16U; index++) {
		uint32_t word = response[3U - index / 4U];

		info->cid[index] = (uint8_t)(word >> (24U - 8U * (index % 4U)));
	}
	info->identity.manufacturer_id = info->cid[0];
	info->identity.oem_id[0] = (char)info->cid[1];
	info->identity.oem_id[1] = (char)info->cid[2];
	info->identity.oem_id[2] = '\0';
	for (uint32_t index = 0U; index < 5U; index++) {
		info->identity.product_name[index] = (char)info->cid[3U + index];
	}
	info->identity.product_name[5] = '\0';
	info->identity.product_revision_major = info->cid[8];
	info->identity.product_revision_minor = info->cid[9];
	info->identity.serial_number = (uint32_t)info->cid[10] << 24 | (uint32_t)info->cid[11] << 16 |
		(uint32_t)info->cid[12] << 8 | info->cid[13];
	info->identity.manufacture_year = (uint16_t)(2000U + (info->cid[14] >> 4));
	info->identity.manufacture_month = info->cid[14] & 0x0fU;
}

/** @brief The tuning block every SD card answers CMD19 with. */
static const uint8_t sd_tuning_block[TUNE_BLOCK_BYTES] = {
	0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc, 0xc3, 0x3c,
	0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef, 0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb, 0xbf, 0xff, 0x7f, 0xff,
	0x77, 0xf7, 0xbd, 0xef, 0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c, 0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef,
	0xff, 0xee, 0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff, 0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde
};

/**
 * @brief Run one CMD19 and check the tuning block.
 *
 * @details The buffer is cache-line friendly but only 16-byte aligned, so the host may take
 * its bounce path; the comparison catches a transfer that completed but sampled outside the
 * card's eye.
 *
 * @return 0 when the tuning block came and is the expected one, or the host error, or -EIO
 *         when the bytes differ.
 */
static int sd_tune_read(void)
{
	uint8_t buffer[TUNE_BLOCK_BYTES] __attribute__((aligned(16)));
	uint32_t response[4];
	int result = sunxi_mmc_command(SD_CMD_SEND_TUNING_BLOCK, 0U, MMC_RESP_R1, response, buffer, 1U,
		TUNE_BLOCK_BYTES, false, 50U);

	if (result != 0) {
		return result;
	}
	return memcmp(buffer, sd_tuning_block, sizeof(sd_tuning_block)) != 0 ? -EIO : 0;
}

/**
 * @brief Find the data sample point of the interface.
 *
 * @details The scan walks every phase and delay of the grid and reads the tuning block
 * TUNE_REPEAT times at each point; a point passes only when all repeats are right. For each
 * phase it records the longest run of passing delays, then counts the phases whose best run is
 * at least half the global maximum ("near best") and takes the middle of those phases, so the
 * chosen point does not sit on an edge. Within the phase it takes the middle of the run, which
 * is the centre of the eye. A run covering the whole grid means every delay works: the
 * interface's own sample point is kept instead of shifting the sampling clock by half a
 * window. The chosen point is then verified TUNE_VERIFY times; any failure restores the default
 * point, recovers the command engine (the failed reads can leave a command in it) and fails.
 * The scan is capped at two seconds and stops at once when the card leaves the slot, because a
 * card that stopped answering turns each read into a one second timeout and the grid is 768
 * reads.
 *
 * @return 0 when a point was chosen and verified, -EIO when there was no useful window or the
 *         verify failed, -ENODEV when the card left the slot, or -ETIMEDOUT when the scan ran
 *         out of time.
 */
static int sd_execute_tuning(void)
{
	uint8_t lengths[TUNE_PHASES] = { 0 };
	uint8_t starts[TUNE_PHASES] = { 0 };
	uint32_t max_length = 0U;
	uint32_t near_best = 0U;
	uint32_t chosen = 0U;
	uint32_t best_phase = 0U;
	uint32_t best_start = 0U;
	uint32_t best_length = 0U;
	int first_error = 0;
	/* A card that stopped answering turns every read into a one second timeout and the grid
	 * is 768 reads: cap the whole scan so identification cannot stall for minutes. */
	uint64_t deadline = ktime_get_ms() + 2000U;
	bool aborted = false;

	sunxi_mmc_set_quiet(true);
	/* phase by phase: find the longest run of passing delays in each */
	for (uint32_t phase = 0U; phase < TUNE_PHASES && !aborted; phase++) {
		uint32_t run = 0U;
		uint32_t run_start = 0U;
		uint32_t row_length = 0U;
		uint32_t row_start = 0U;

		for (uint32_t delay = 0U; delay < TUNE_DELAYS; delay++) {
			bool pass = true;
			int error = 0;

			if (!sunxi_mmc_present()) {
				/* the card left the slot: stop at once and fall back */
				if (first_error == 0) {
					first_error = -ENODEV;
				}
				aborted = true;
				break;
			}
			if (ktime_get_ms() > deadline) {
				if (first_error == 0) {
					first_error = -ETIMEDOUT;
				}
				aborted = true;
				break;
			}
			sunxi_mmc_tune_point(phase, delay);
			for (uint32_t repeat = 0U; repeat < TUNE_REPEAT; repeat++) {
				error = sd_tune_read();
				if (error != 0) {
					pass = false;
					break;
				}
			}
			if (!pass && first_error == 0) {
				first_error = error;
			}
			/* keep the longest run, and where it started */
			if (pass) {
				if (run == 0U) {
					run_start = delay;
				}
				run++;
				if (run > row_length) {
					row_length = run;
					row_start = run_start;
				}
			} else {
				run = 0U;
			}
		}
		lengths[phase] = (uint8_t)row_length;
		starts[phase] = (uint8_t)row_start;
		max_length = MAX(max_length, row_length);
	}

	/* count the phases that are about as good as the best one */
	for (uint32_t phase = 0U; phase < TUNE_PHASES; phase++) {
		if (lengths[phase] * 2U >= max_length) {
			near_best++;
		}
	}
	/* and take the middle of those phases */
	for (uint32_t phase = 0U; phase < TUNE_PHASES; phase++) {
		if (lengths[phase] * 2U >= max_length && chosen++ == (near_best - 1U) / 2U) {
			best_phase = phase;
			best_start = starts[phase];
			best_length = lengths[phase];
		}
	}
	if (best_length < TUNE_MIN_WINDOW) {
		sunxi_mmc_set_quiet(false);
		saved_tune_valid = false;
		sunxi_mmc_tune_point(0U, 0U);
		/* the failed tuning reads can leave a command in the engine */
		(void)sunxi_mmc_recover();
		pr_warn("tuning found no window (widest %u, first error %d)\n", (uint32_t)max_length,
			first_error);
		return -EIO;
	}
	/* A window as wide as the grid means every delay reads the block: keep the interface's
	 * own sample point then, instead of shifting the sampling clock by half a window. */
	if (best_length >= TUNE_DELAYS) {
		best_phase = 0U;
		best_start = 0U;
		best_length = 0U;
	}
	/* the middle of the window is the centre of the eye */
	sunxi_mmc_tune_point(best_phase, best_start + best_length / 2U);
	for (uint32_t verify = 0U; verify < TUNE_VERIFY; verify++) {
		if (sd_tune_read() != 0) {
			sunxi_mmc_set_quiet(false);
			saved_tune_valid = false;
			sunxi_mmc_tune_point(0U, 0U);
			(void)sunxi_mmc_recover();
			pr_warn("tuning: the chosen point failed the verify reads\n");
			return -EIO;
		}
	}
	sunxi_mmc_set_quiet(false);
	saved_tune_phase = best_phase;
	saved_tune_delay = best_start + best_length / 2U;
	saved_tune_valid = true;
	pr_info("tuned: phase %u, delay %u (window %u)\n", (uint32_t)best_phase,
		(uint32_t)saved_tune_delay, (uint32_t)best_length);
	return 0;
}

/**
 * @brief Move a range of blocks, splitting it into commands of at most 256 blocks.
 *
 * @details Each chunk is one single- or multi-block command, with the byte address used for a
 * standard-capacity card and the sector address for a high-capacity one. A failure at a clock
 * above 52 MHz is retried once after a host recovery and a fresh tuning pass, because a UHS
 * transfer that fails once may leave the controller wedged. The sector and buffer advance
 * after each successful chunk, so a later chunk failing leaves the earlier ones done.
 *
 * @param[in] card   Identified card.
 * @param[in] sector First sector.
 * @param[in] buffer Data buffer.
 * @param[in] count  Number of sectors.
 * @param[in] write  true to write, false to read.
 * @return 0 on success, or the error that stopped the transfer.
 */
static int sd_transfer(struct mmc_card *card, uint64_t sector, void *buffer, uint32_t count,
	bool write)
{
	uint32_t response[4];
	int result = -EIO;

	while (count != 0U) {
		uint32_t blocks = count < SD_MAX_BLOCKS ? count : SD_MAX_BLOCKS;
		uint32_t argument = card->info.high_capacity ? (uint32_t)sector : (uint32_t)(sector * 512U);
		uint32_t opcode = write ? (blocks > 1U ? SD_CMD_WRITE_MULTIPLE : SD_CMD_WRITE_SINGLE) :
					  (blocks > 1U ? SD_CMD_READ_MULTIPLE : SD_CMD_READ_SINGLE);

		result = sunxi_mmc_command(opcode, argument, MMC_RESP_R1, response, buffer, blocks, 512U,
			write, 1000U);
		if (result != 0 && card->info.clock > 52000000U) {
			/* high speed: reset the controller and find the sample point again */
			(void)sunxi_mmc_recover();
			if (saved_tune_valid) {
				(void)sd_execute_tuning();
			}
			result = sunxi_mmc_command(opcode, argument, MMC_RESP_R1, response, buffer, blocks,
				512U, write, 1000U);
		}
		if (result != 0) {
			break;
		}
		sector += blocks;
		buffer = (uint8_t *)buffer + blocks * 512U;
		count -= blocks;
	}
	return result;
}

/**
 * @brief Read blocks from the card.
 *
 * @param[in]  card   Identified card.
 * @param[in]  sector First sector.
 * @param[out] buffer Destination buffer.
 * @param[in]  count  Number of sectors.
 * @return 0 on success, -ENODEV when the card is not ready, or a transfer error.
 */
int sd_read(struct mmc_card *card, uint64_t sector, void *buffer, uint32_t count)
{
	if (card == NULL || !card->ready) {
		return -ENODEV;
	}
	return sd_transfer(card, sector, buffer, count, false);
}

/**
 * @brief Write blocks to the card.
 *
 * @param[in] card   Identified card.
 * @param[in] sector First sector.
 * @param[in] buffer Source buffer.
 * @param[in] count  Number of sectors.
 * @return 0 on success, -ENODEV when the card is not ready, or a transfer error.
 */
int sd_write(struct mmc_card *card, uint64_t sector, const void *buffer, uint32_t count)
{
	if (card == NULL || !card->ready) {
		return -ENODEV;
	}
	return sd_transfer(card, sector, (void *)buffer, count, true);
}

/**
 * @brief Wait until the card has finished the writes it accepted.
 *
 * @details SD: a write is complete when the card released busy; nothing is held back. A single
 * status command is therefore enough, with the same high-speed recovery and retry as a data
 * transfer.
 *
 * @param[in] card Identified card.
 * @return 0 on success, -ENODEV when the card is not ready, or -EIO when the status poll failed.
 */
int sd_flush(struct mmc_card *card)
{
	uint32_t response[4];
	int result;

	if (card == NULL || !card->ready) {
		return -ENODEV;
	}
	/* SD: a write is complete when the card released busy; nothing is held back */
	result = sd_command(SD_CMD_SEND_STATUS, card_rca << 16, MMC_RESP_R1, response);
	if (result != 0 && card->info.clock > 52000000U) {
		(void)sunxi_mmc_recover();
		if (saved_tune_valid) {
			(void)sd_execute_tuning();
		}
		result = sd_command(SD_CMD_SEND_STATUS, card_rca << 16, MMC_RESP_R1, response);
	}
	return result == 0 ? 0 : -EIO;
}

/**
 * @brief Run the identification sequence at one signalling level.
 *
 * @details The sequence is the SD part 1 order: CMD0 to idle at 400 kHz on one data line,
 * CMD8 to tell a v2 card apart and check the supply, ACMD41 polling until the card reports
 * ready (with the high-capacity and 1.8 V request bits), CMD11 if both sides agreed on 1.8 V,
 * CMD2 for the CID, CMD3 for the RCA, CMD9 for the capacity, CMD7 to select the card and
 * ACMD6 plus the host width register for four data lines. A standard-capacity card also needs
 * CMD16 to fix the block length at 512 bytes. The speed mode is then chosen from the CMD6
 * support word: SDR104 at 200 or 100 MHz, SDR50 at 100 MHz, or SDR25 at 50 MHz, each switch
 * made at 25 MHz and followed by a tuning pass where the mode needs one. Falling back, the
 * card stays at 25 MHz.
 *
 * @param[in,out] card       Card state to fill.
 * @param[in]     already_1v8 true when the caller already set the 1.8 V supply, so the 1.8 V
 *                            request bit is not sent and the UHS modes are allowed.
 * @return 0 on success, or the error of the step that failed.
 */
static int sd_identify_once(struct mmc_card *card, bool already_1v8)
{
	uint32_t response[4];
	uint32_t ocr = 0U;
	uint32_t uhs_request;
	int result;
	uint64_t deadline;

	card_rca = 0U;
	memset(&card->info, 0, sizeof(card->info));

	/* identification always starts slow and narrow */
	result = sunxi_mmc_set_clock(400000U);
	if (result != 0) {
		return result;
	}
	sunxi_mmc_set_width(1U);

	result = sd_command(SD_CMD_GO_IDLE, 0U, MMC_RESP_NONE, response);
	if (result != 0) {
		mmc_note_error("CMD0 %s", result == -ETIMEDOUT ? "Timeout" : "Error");
		pr_err("CMD0 failed: %d\n", result);
		return result;
	}
	/* CMD8 tells an SD v2 card apart and checks the 2.7-3.6 V range */
	result = sd_command(SD_CMD_SEND_IF_COND, 0x1aaU, MMC_RESP_R7, response);
	if (result == 0 && (response[0] & 0xfffU) != 0x1aaU) {
		mmc_note_error("CMD8 echo error");
		pr_err("CMD8 answered 0x%08x\n", (uint32_t)response[0]);
		return -EIO;
	}
	/* ask a version 2 card whether it takes 1.8 V signalling (S18R); the answer is S18A */
	uhs_request = !already_1v8 && result == 0 ? (1U << 24) : 0U;
	deadline = ktime_get_ms() + 1000U;
	for (;;) {
		/* HCS asks for high capacity, the voltage window covers 3.3 V */
		result = sd_app_command(SD_ACMD_SEND_OP_COND, 0x40ff8000U /* HCS, 3.3 V */ | uhs_request,
			MMC_RESP_R3, response);
		if (result != 0) {
			mmc_note_error("ACMD41 %s", result == -ETIMEDOUT ? "Timeout" : "Error");
			pr_err("ACMD41 -> %d\n", result);
			return result;
		}
		ocr = response[0];
		if ((ocr & 0x80000000U) != 0U) {
			break;
		}
		if (ktime_get_ms() > deadline) {
			mmc_note_error("ACMD41 Timeout (OCR %08x)", (uint32_t)ocr);
			pr_err("ACMD41 timeout (OCR 0x%08x)\n", (uint32_t)ocr);
			return -ETIMEDOUT;
		}
		msleep(1U);
	}
	card->info.high_capacity = (ocr & 0x40000000U) != 0U;
	card->info.s18a = uhs_request != 0U && (ocr & (1U << 24)) != 0U;
	if (card->info.s18a) {
		result = sd_switch_to_1v8();
		if (result != 0) {
			return result;
		}
	}

	/* CMD2 is answered by every card at once; give the bus time to settle */
	for (uint32_t attempt = 0U; attempt < 3U; attempt++) {
		msleep(10U);
		result = sd_command(SD_CMD_ALL_SEND_CID, 0U, MMC_RESP_R2, response);
		if (result == 0) {
			break;
		}
		pr_err("CMD2 -> %d\n", result);
	}
	if (result != 0) {
		return result;
	}
	sd_decode_cid(response, &card->info);

	result = sd_command(SD_CMD_SEND_RCA, 0U, MMC_RESP_R6, response);
	if (result != 0) {
		pr_err("CMD3 -> %d\n", result);
		return result;
	}
	card_rca = response[0] >> 16;

	/* the CSD carries the capacity, in one of two encodings */
	result = sd_command(SD_CMD_SEND_CSD, card_rca << 16, MMC_RESP_R2, response);
	if (result != 0) {
		return result;
	}
	if (((response[3] >> 30) & 3U) == 1U) {
		/* v2 CSD: capacity in 1 KiB units */
		uint32_t csize = ((response[2] & 0x3fU) << 16) | (response[1] >> 16);

		card->info.sectors = (csize + 1U) * 1024U;
	} else {
		/* v1 CSD: csize, multiplier and read block length */
		uint32_t csize = ((response[2] & 0x3ffU) << 2) | (response[1] >> 30);
		uint32_t multiplier = (response[1] >> 15) & 7U;
		uint32_t read_block = (response[2] >> 16) & 0xfU;

		card->info.sectors = (csize + 1U) * (1U << (multiplier + 2U)) * (1U << read_block) / 512U;
	}

	result = sd_command(SD_CMD_SELECT_CARD, card_rca << 16, MMC_RESP_R1, response);
	if (result != 0) {
		return result;
	}
	/* bus width: four lines, card and controller side */
	result = sd_app_command(SD_ACMD_SET_BUS_WIDTH, 2U, MMC_RESP_R1, response);
	if (result != 0) {
		return result;
	}
	sunxi_mmc_set_width(4U);
	card->info.bus_width = 4;

	if (!card->info.high_capacity) {
		result = sd_command(SD_CMD_SET_BLOCKLEN, 512U, MMC_RESP_R1, response);
		if (result != 0) {
			return result;
		}
	}
	/* the card's CMD6 support word, and the fastest mode both sides take */
	{
		/** @brief Speed modes tried in order, fastest first. */
		static const struct {
			/** @brief Mode name shown to the user. */
			const char *name;
			/** @brief Function number in the CMD6 access-mode group. */
			uint32_t function;
			/** @brief Support bit in the CMD6 support word. */
			uint32_t support_bit;
			/** @brief Card clock the mode runs at. */
			uint32_t clock;
			/** @brief Whether the mode needs a tuning pass. */
			bool tune;
		} modes[] = {
			{ "SDR104", 3U, 3U, 200000000U, true },
			{ "SDR104", 3U, 3U, 100000000U, true },
			{ "SDR50", 2U, 2U, 100000000U, true },
			{ "SDR25", 1U, 1U, 50000000U, false },
		};
		uint8_t status[64] = { 0 };
		bool uhs = already_1v8 || card->info.s18a;

		if (sd_switch(0x00fffff0U, status) == 0) {
			card->info.sw_groups[0] = (uint16_t)((uint16_t)status[12] << 8 | status[13]);
			card->info.max_current = (uint16_t)((uint16_t)status[0] << 8 | status[1]);
		}
		for (uint32_t index = 0U; index < ARRAY_SIZE(modes); index++) {
			/* the UHS modes need the 1.8 V signalling the switch above provides */
			if ((card->info.sw_groups[0] & (1U << modes[index].support_bit)) == 0U ||
			    (modes[index].function != 1U && !uhs)) {
				continue;
			}
			/* the access mode switch itself runs at a low clock */
			if (sunxi_mmc_set_clock(25000000U) != 0 ||
			    sd_switch(0x80fffff0U | modes[index].function, status) != 0 ||
			    sunxi_mmc_set_clock(modes[index].clock) != 0) {
				continue;
			}
			if (modes[index].tune && sd_execute_tuning() != 0) {
				(void)sunxi_mmc_set_clock(25000000U);
				continue;
			}
			card->info.mode = modes[index].name;
			card->info.clock = modes[index].clock;
			card->info.tuned = modes[index].tune;
			break;
		}
	}
	if (card->info.clock == 0U) {
		card->info.mode = card->info.s18a ? "SDR12" : "default";
		card->info.clock = 25000000U;
		(void)sunxi_mmc_set_clock(25000000U);
	}
	return 0;
}

/** @brief Last card failure, shown verbatim by the error UI. */
static char last_error[48];

/**
 * @brief Format and store the last card failure.
 *
 * @param[in] format printf-style format string.
 * @param[in] ...    Format arguments.
 */
void mmc_note_error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_error, sizeof(last_error), format, arguments);
	va_end(arguments);
}

/**
 * @brief The last card failure recorded by mmc_note_error().
 *
 * @return A NUL-terminated description, empty when nothing failed.
 */
const char *mmc_last_error(void)
{
	return last_error;
}

/**
 * @brief Forget the recorded failure, so the UI does not show a stale one.
 */
void mmc_last_error_clear(void)
{
	last_error[0] = '\0';
}

/**
 * @brief Identify the card, trying both signalling levels.
 *
 * @details 3.3 V first; a card left in 1.8 V signalling by an earlier run answers only there,
 * so a failed attempt clears the info, sets the 1.8 V supply and runs the sequence again in
 * that mode. The controller's clock is returned to the identification rate first, because the
 * failed attempt may have left a different divider programmed.
 *
 * @param[in,out] card Card state to fill.
 * @return 0 on success, -EINVAL for a NULL @p card, or the error of the last attempt.
 */
int sd_identify(struct mmc_card *card)
{
	int result;

	if (card == NULL) {
		return -EINVAL;
	}
	/* 3.3 V first; a card left in 1.8 V signalling by an earlier run answers only there */
	sunxi_mmc_set_voltage(3300U);
	result = sd_identify_once(card, false);
	if (result != 0) {
		pr_info("no answer at 3.3 V, trying 1.8 V\n");
		memset(&card->info, 0, sizeof(card->info));
		sunxi_mmc_set_voltage(1800U);
		(void)sunxi_mmc_set_clock(400000U);
		result = sd_identify_once(card, true);
	}
	return result;
}
