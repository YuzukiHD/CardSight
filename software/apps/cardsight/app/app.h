// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file app.h
 * @brief Public application state and the request message the UI sends.
 *
 * The UI thread only reads a snapshot of @c cardsight_state and submits
 * requests; the worker task in app.c performs every card operation. The types
 * here are the contract between the two.
 */
#ifndef CARDSIGHT_APP_H
#define CARDSIGHT_APP_H
#include "storage/storage_proxy.h"
#include <hal/blkdev.h>
#include <hal/mmc/core.h>
#include <vfs.h>
/** @brief Maximum number of partitions a layout request may describe. */
#define APP_LAYOUT_MAX 8U
/**
 * @brief Number of directory entries the browser keeps.
 *
 * The browser keeps one screenful of names; larger directories report how many were left out.
 */
#define APP_FILE_ENTRIES 64U
/** @brief Longest file name, including the terminating NUL, held in a listing. */
#define APP_FILE_NAME 128U

/**
 * @brief One directory entry shown in the file browser.
 */
struct cardsight_file_entry {
	/** @brief Name as the source file system reports it. */
	char name[APP_FILE_NAME];
	/** @brief File size in bytes; zero for a directory. */
	uint64_t size;
	/** @brief True when the entry is a directory. */
	bool dir;
};

/**
 * @brief The listing of one directory, valid or carrying the error that prevented it.
 */
struct cardsight_file_list {
	/** @brief Entries kept by the browser, directories first, then case-insensitive name order. */
	struct cardsight_file_entry entries[APP_FILE_ENTRIES];
	/** @brief Path of the listed directory, relative to the partition root. */
	char path[VFS_PATH_MAX];
	/** @brief Number of valid entries in @c entries. */
	size_t count;
	/** @brief Total number of entries seen, including those dropped because the array was full. */
	size_t total;
	/** @brief Partition number the listing belongs to. */
	uint32_t partition;
	/** @brief Error that stopped the listing, or zero. */
	int error;
	/** @brief True when @c total exceeds @c count. */
	bool truncated;
	/** @brief True once the listing has been filled, even when it holds only an error. */
	bool valid;
};

/**
 * @brief Operation the worker can be asked to perform; one is in flight at a time.
 */
enum cardsight_request_type {
	/** @brief Re-read the card, its table and its partitions. */
	CARDSIGHT_REQUEST_SCAN,
	/** @brief Expose the whole card over USB mass storage. */
	CARDSIGHT_REQUEST_START_DIRECT,
	/** @brief Start the exFAT proxy view and expose it over USB. */
	CARDSIGHT_REQUEST_START_PROXY,
	/** @brief Stop USB mass storage and the proxy. */
	CARDSIGHT_REQUEST_STOP,
	/** @brief Format one partition. */
	CARDSIGHT_REQUEST_FORMAT,
	/** @brief Write a new partition table and format its partitions. */
	CARDSIGHT_REQUEST_LAYOUT,
	/** @brief List one directory of one partition. */
	CARDSIGHT_REQUEST_LIST,
};

/**
 * @brief One request queued to the worker task.
 *
 * The structure is passed by value through the message queue; only the fields
 * relevant to @c type are read by the worker.
 */
struct cardsight_request {
	/** @brief Operation to perform. */
	enum cardsight_request_type type;
	/** @brief Partition number for format and list requests. */
	uint32_t partition;
	/** @brief File system to create for a format or layout request. */
	enum blk_partition_content content;
	/** @brief Partition table type to write for a layout request. */
	enum blk_partition_table_type table;
	/** @brief Number of valid entries in @c entries for a layout request. */
	uint32_t count;
	/** @brief True for a full format, which erases the partition before writing the file system. */
	bool full;
	/** @brief Bit mask of partitions to expose, bit zero being partition one. */
	uint32_t selected;
	/** @brief Directory path for a list request. */
	char path[VFS_PATH_MAX];
	/** @brief Partitions of a layout request. */
	struct blk_partition_layout_entry entries[APP_LAYOUT_MAX];
};

/**
 * @brief Everything the worker publishes to the UI.
 *
 * Protected by @c app_lock. The UI copies it with @c cardsight_snapshot(); the
 * worker is the only writer.
 */
struct cardsight_state {
	/** @brief Partition information from the last scan. */
	struct storage_proxy_partition_info partitions[STORAGE_PROXY_MAX_PARTITIONS];
	/** @brief Identity of the card in the slot, when it could be read. */
	struct mmc_card_info card;
	/** @brief Latest directory listing. */
	struct cardsight_file_list files;
	/** @brief Number of valid entries in @c partitions. */
	size_t partition_count;
	/** @brief Capacity of the card in 512-byte sectors. */
	uint64_t sectors;
	/** @brief Error of the last completed operation, or zero. */
	int error;
	/** @brief Operation currently running or last run. */
	enum cardsight_request_type request;
	/** @brief Partition table type found on the card. */
	enum blk_partition_table_type table;
	/** @brief True when a block device for the source disk could be opened. */
	bool card_present;
	/** @brief True while the MMC slot is identifying a newly inserted card. */
	bool card_detecting;
	/** @brief True when the card is not read-only. */
	bool card_writable;
	/** @brief True when @c card holds a valid identity. */
	bool card_info_valid;
	/** @brief True while the proxy view and its USB device are up. */
	bool proxy_active;
	/** @brief True while the worker runs a request. */
	bool busy;
	/** @brief True when the host has configured the USB mass storage device. */
	bool configured;
	/** @brief Bytes the host has read over USB since the device started. */
	uint64_t bytes_read;
	/** @brief Bytes the host has written over USB since the device started. */
	uint64_t bytes_written;
	/** @brief Read throughput of the last second, in bytes per second. */
	uint32_t read_rate;
	/** @brief Write throughput of the last second, in bytes per second. */
	uint32_t write_rate;
	/** @brief Human-readable description of the current or last operation. */
	char operation[48];
	/** @brief Human-readable detail of the last card failure. */
	char card_error[48];
	/** @brief Progress percentage of the current operation, or -1 when it is not measurable. */
	int progress;
	/** @brief True while the USB mass storage device is running. */
	bool running;
};
/**
 * @brief Initialise the state, the locks and the worker task.
 */
void cardsight_app_init(void);
/**
 * @brief Copy the shared state for the UI to render.
 *
 * @param[out] state Destination snapshot; @c running is refreshed from the USB
 *                   driver at copy time.
 */
void cardsight_snapshot(struct cardsight_state *state);
/**
 * @brief Queue one request for the worker.
 *
 * @param[in] request Request to copy into the queue; ignored when the worker is
 *                    busy or the queue is full.
 */
void send_request(const struct cardsight_request *request);
#endif
