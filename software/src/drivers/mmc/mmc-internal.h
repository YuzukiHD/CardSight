/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file mmc-internal.h
 * @brief The contract inside the MMC subsystem.
 *
 * The SD protocol layer (core/sd.c) talks to the controller through these calls and never
 * touches a register, and the block layer (card/mmcblk.c) drives the card lifecycle through
 * sd_*(). Tuning is part of the protocol: the controller only offers a sample point and the
 * command engine.
 */

#ifndef CARDSIGHT_MMC_INTERNAL_H
#define CARDSIGHT_MMC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Board data of the host controller; defined in host/sunxi-mmc.h. */
struct sunxi_mmc_config;
/** @brief State of one card; defined in <hal/mmc/card.h>. */
struct mmc_card;

/**
 * @brief Shape of the response a command expects from the card.
 */
enum mmc_response {
	/** @brief No response at all (a broadcast such as CMD0). */
	MMC_RESP_NONE,
	/** @brief 48-bit R1: the card status word, CRC checked. */
	MMC_RESP_R1,
	/** @brief 48-bit R1b: R1 followed by a busy period on DAT0. */
	MMC_RESP_R1B,
	/** @brief 136-bit R2: CID or CSD, no CRC on the wire. */
	MMC_RESP_R2,
	/** @brief 48-bit R3: the OCR, CRC not checked. */
	MMC_RESP_R3,
	/** @brief 48-bit R6: the RCA the card published, CRC checked. */
	MMC_RESP_R6,
	/** @brief 48-bit R7: the CMD8 interface-condition echo, CRC checked. */
	MMC_RESP_R7,
};

/* ---- host/sunxi-mmc.c: the SMHC0 controller ---- */

/**
 * @brief Bring the controller to a known state and prepare the card port.
 *
 * @details Programs the port pins, enables the bus clock and releases the reset, then resets
 * the controller, clears the interrupt status and picks a one-bit bus at a slow module clock,
 * so the identification sequence can start.
 *
 * @param[in] config Board data for the slot.
 * @return 0 on success, -EINVAL for a NULL @p config or -ETIMEDOUT when the reset never clears.
 */
int sunxi_mmc_host_init(const struct sunxi_mmc_config *config);

/**
 * @brief Set the card clock and the timing of the data interface.
 *
 * @details Stops the card clock, reprograms the module clock divider, starts the clock again
 * and then sets the drive and sample delays that match the requested rate: half a cycle of
 * command drive, plus the data drive shift and the built-in sample point for the 26-52 MHz
 * range. Above 52 MHz the interface runs fast and the sample point is left to tuning.
 *
 * @param[in] hz Requested card clock in Hz.
 * @return 0 on success, or a negative errno when the clock cannot be stopped or started.
 */
int sunxi_mmc_set_clock(uint32_t hz);

/**
 * @brief Select the bus width of the card interface.
 *
 * @param[in] width Number of data lines: 1, 4 or 8.
 */
void sunxi_mmc_set_width(uint32_t width);

/**
 * @brief Set the I/O supply of the card port.
 *
 * @details Writes the pad voltage control and waits for the status bit to report the new
 * level before the caller restarts the card clock.
 *
 * @param[in] millivolt 3300 or 1800.
 */
void sunxi_mmc_set_voltage(uint32_t millivolt);

/**
 * @brief The host half of the CMD11 sequence.
 *
 * @details Stops the card clock with the controller taking part in the voltage switch,
 * changes the supply, waits 10 ms and starts the clock again with the same flag.
 *
 * @param[in] millivolt 3300 or 1800.
 * @return 0 on success, or a negative errno from either clock update.
 */
int sunxi_mmc_voltage_switch(uint32_t millivolt);

/**
 * @brief Report whether the card holds DAT0 low.
 *
 * @return true when the card is busy.
 */
bool sunxi_mmc_card_busy(void);

/**
 * @brief Report whether a card is in the slot.
 *
 * @details Returns true unconditionally when the board has no card detect pin configured.
 *
 * @return true when the detect pin reads active low.
 */
bool sunxi_mmc_present(void);

/**
 * @brief Run one command, optionally with a data phase.
 *
 * @details The response words are the controller's response registers, most significant word
 * last. Data goes through the internal DMA engine when it is block sized, otherwise through
 * the FIFO.
 *
 * @param[in]  opcode        Command index, six bits.
 * @param[in]  argument      Command argument.
 * @param[in]  response_kind Expected response shape.
 * @param[out] response      Four words receiving the response.
 * @param[in,out] buffer     Data buffer, or NULL for a command without data.
 * @param[in]  blocks        Number of blocks in the data phase.
 * @param[in]  block_size    Bytes per block.
 * @param[in]  write         true to write to the card, false to read.
 * @param[in]  timeout_ms    Extra time on top of the built-in one second.
 * @return 0 on success, -ETIMEDOUT or -EIO when the card or the DMA engine reported a failure.
 */
int sunxi_mmc_command(uint32_t opcode, uint32_t argument, enum mmc_response response_kind,
	uint32_t response[4], void *buffer, uint32_t blocks, uint32_t block_size, bool write,
	uint32_t timeout_ms);

/**
 * @brief Reset the controller and restore the clock and bus width it had.
 *
 * @details Used after a failed transfer left the command engine or the DMA engine wedged.
 *
 * @return 0 on success, or a negative errno from restoring the clock.
 */
int sunxi_mmc_recover(void);

/**
 * @brief Move the data sample point of the card interface.
 *
 * @param[in] phase Sampling phase, 0 to 2.
 * @param[in] delay Sampling delay in the controller's delay units.
 */
void sunxi_mmc_tune_point(uint32_t phase, uint32_t delay);

/**
 * @brief Record the last card failure for the error UI, e.g. "CMD5 Read Timeout".
 *
 * @param[in] format printf-style format string.
 * @param[in] ...    Format arguments.
 */
void mmc_note_error(const char *format, ...);

/**
 * @brief Silence the log of expected failures during a tuning scan.
 *
 * @details The scan reads hundreds of blocks on purpose; the failed reads are not logged.
 *
 * @param[in] quiet true to suppress the per-transfer debug lines.
 */
void sunxi_mmc_set_quiet(bool quiet);

/**
 * @brief Report how many transfers used the DMA engine directly and how many bounced.
 *
 * @param[out] direct Number of transfers that used the caller's buffer.
 * @param[out] bounce Number of transfers that went through the bounce buffer.
 */
void sunxi_mmc_dma_stats(uint32_t *direct, uint32_t *bounce);

/* ---- core/sd.c: the SD protocol ---- */

/**
 * @brief Identify the card and bring it to its fastest shared mode.
 *
 * @details Fills @c card->info and sets @c card->ready when the sequence succeeds. The first
 * attempt runs at 3.3 V; a card that does not answer is tried again at 1.8 V, because an
 * earlier run may have left it in 1.8 V signalling.
 *
 * @param[in,out] card Card state to fill.
 * @return 0 on success, or a negative errno describing the failing step.
 */
int sd_identify(struct mmc_card *card);

/**
 * @brief Read blocks from the card.
 *
 * @param[in]  card   Identified card.
 * @param[in]  sector First sector, 512 bytes each.
 * @param[out] buffer Destination buffer.
 * @param[in]  count  Number of sectors.
 * @return 0 on success, or a negative errno.
 */
int sd_read(struct mmc_card *card, uint64_t sector, void *buffer, uint32_t count);

/**
 * @brief Write blocks to the card.
 *
 * @param[in] card   Identified card.
 * @param[in] sector First sector, 512 bytes each.
 * @param[in] buffer Source buffer.
 * @param[in] count  Number of sectors.
 * @return 0 on success, or a negative errno.
 */
int sd_write(struct mmc_card *card, uint64_t sector, const void *buffer, uint32_t count);

/**
 * @brief Wait until the card has finished the writes it accepted.
 *
 * @param[in] card Identified card.
 * @return 0 on success, or a negative errno.
 */
int sd_flush(struct mmc_card *card);

#endif /* CARDSIGHT_MMC_INTERNAL_H */
