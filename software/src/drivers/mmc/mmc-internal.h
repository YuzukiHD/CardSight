/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The contract inside the MMC subsystem: the SD protocol layer (core/sd.c) talks to the
 * controller through these calls and never touches a register, and the block layer
 * (card/mmcblk.c) drives the card lifecycle through sd_*(). Tuning is part of the protocol:
 * the controller only offers a sample point and the command engine.
 */

#ifndef CARDSIGHT_MMC_INTERNAL_H
#define CARDSIGHT_MMC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

struct sunxi_mmc_config;
struct mmc_card;

enum mmc_response {
	MMC_RESP_NONE,
	MMC_RESP_R1,
	MMC_RESP_R1B,
	MMC_RESP_R2,
	MMC_RESP_R3,
	MMC_RESP_R6,
	MMC_RESP_R7,
};

/* ---- host/sunxi-mmc.c: the SMHC0 controller ---- */

int sunxi_mmc_host_init(const struct sunxi_mmc_config *config);

/* Card clock and bus width; the controller restores them after a reset. */
int sunxi_mmc_set_clock(uint32_t hz);
void sunxi_mmc_set_width(uint32_t width);

/* The I/O supply of the card port, 1800 or 3300 mV. */
void sunxi_mmc_set_voltage(uint32_t millivolt);

/*
 * The host half of the CMD11 sequence: stop the card clock with the controller taking part in
 * the voltage switch, change the supply, start the clock again.
 */
int sunxi_mmc_voltage_switch(uint32_t millivolt);

/* The card holds DAT0 low (it is busy). */
bool sunxi_mmc_card_busy(void);

bool sunxi_mmc_present(void);

/*
 * One command, optionally with a data phase. The response words are the controller's response
 * registers, most significant word last. Data goes through the internal DMA engine when it is
 * block sized, otherwise through the FIFO.
 */
int sunxi_mmc_command(uint32_t opcode, uint32_t argument, enum mmc_response response_kind,
	uint32_t response[4], void *buffer, uint32_t blocks, uint32_t block_size, bool write,
	uint32_t timeout_ms);

/* Reset the controller and restore the clock and bus width it had. */
int sunxi_mmc_recover(void);

/* Data sample point of the card interface: a phase and a sampling delay. */
void sunxi_mmc_tune_point(uint32_t phase, uint32_t delay);

/* The scan reads hundreds of blocks on purpose; the expected failures are not logged. */
void sunxi_mmc_set_quiet(bool quiet);

void sunxi_mmc_dma_stats(uint32_t *direct, uint32_t *bounce);

/* ---- core/sd.c: the SD protocol ---- */

/* Identify the card and bring it to its fastest shared mode; fills card->info and card->ready. */
int sd_identify(struct mmc_card *card);
int sd_read(struct mmc_card *card, uint64_t sector, void *buffer, uint32_t count);
int sd_write(struct mmc_card *card, uint64_t sector, const void *buffer, uint32_t count);
int sd_flush(struct mmc_card *card);

#endif /* CARDSIGHT_MMC_INTERNAL_H */
