// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CARDSIGHT_APP_H
#define CARDSIGHT_APP_H
#include "storage/storage_proxy.h"
#include <hal/blkdev.h>
#include <hal/mmc/core.h>
#define APP_LAYOUT_MAX 8U
enum cardsight_request_type {
	CARDSIGHT_REQUEST_SCAN,
	CARDSIGHT_REQUEST_START_DIRECT,
	CARDSIGHT_REQUEST_START_PROXY,
	CARDSIGHT_REQUEST_STOP,
	CARDSIGHT_REQUEST_FORMAT,
	CARDSIGHT_REQUEST_LAYOUT,
};

struct cardsight_request {
	enum cardsight_request_type type;
	unsigned int partition;
	enum blk_partition_content content;
	enum blk_partition_table_type table;
	unsigned int count;
	bool full;
	uint32_t selected;
	struct blk_partition_layout_entry entries[APP_LAYOUT_MAX];
};

struct cardsight_state {
	struct storage_proxy_partition_info partitions[STORAGE_PROXY_MAX_PARTITIONS];
	struct mmc_card_info card;
	size_t partition_count;
	uint64_t sectors;
	int error;
	enum blk_partition_table_type table;
	bool card_present;
	bool card_writable;
	bool card_info_valid;
	bool proxy_active;
	bool busy;
	bool configured;
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint32_t read_rate;
	uint32_t write_rate;
	char operation[48];
	int progress;
	bool running;
};
void cardsight_app_init(void);
void cardsight_snapshot(struct cardsight_state *state);
void send_request(const struct cardsight_request *request);
#endif
