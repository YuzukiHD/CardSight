// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file app_worker.c
 * @brief The request queue worker that owns every card operation.
 *
 * @details Part of the CardSight application. The shared model is declared in
 * @c app/app_private.h; the UI is refreshed through the LVGL port.
 */
#include "app/app_private.h"
#include "storage/proxy_lazy.h"

/**
 * @brief Translate an operation result into the text shown to the user.
 *
 * @param[in] result Result code of the operation.
 * @return Localized message describing the result.
 */
static const char *failure_text(int result)
{
	switch (result) {
	case 0: return "操作完成";
	case -ENOSPC: return "空间不足";
	case -EBUSY: return "请先停止USB";
	case -EROFS: return "存储卡为只读";
	case -EACCES: return "没有访问权限";
	case -ENODEV: return "未找到存储卡";
	case -ENOMEM: return "内存不足";
	case -EIO: return "存储卡读写错误";
	case -EINVAL: return "分区表无法识别";
	case -ENODATA: return "没有可读取的数据";
	case -ENOENT: return "文件或目录不存在";
	case -ENOTSUP: return "不支持的文件系统";
	case -ETIMEDOUT: return "等待存储卡超时";
	default: return "操作失败";
	}
}


/**
 * @brief Scan the card and report the first volume that failed to probe.
 *
 * @details A card whose volumes all fail to probe is inserted but unreadable:
 * report the first failure with its device name so the sheet can say which
 * partition it was. The partition statuses are inspected under @c app_lock
 * after @c card_scan() has published them.
 *
 * @param[out] detail Buffer receiving "<device> <status>" for the failing volume.
 * @param[in]  size   Size of @p detail in bytes.
 * @return 0 on success, or a negative errno describing the first failure.
 */
static int card_scan_check(char *detail, size_t size)
{
	int result = card_scan();

	if (result != 0) {
		return result;
	}
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	if (app.partition_count > 0U) {
		bool usable = false;

		for (size_t index = 0U; index < app.partition_count; index++) {
			if (app.partitions[index].status == 0) {
				usable = true;
				break;
			}
		}
		if (!usable) {
			result = app.partitions[0].status != 0 ? app.partitions[0].status : -EIO;
			snprintf(detail, size, "%s %d", app.partitions[0].device,
				app.partitions[0].status);
		}
	}
	mutex_unlock(&app_lock);
	return result;
}


/**
 * @brief Queue one request for the worker and mark the application busy.
 *
 * @details The state lock is taken only to claim the busy flag and record the
 * request, then released before touching the queue. When the queue rejects the
 * request the busy flag is cleared again and the reason is published, so the
 * UI never waits on a request that will not run. A refresh is posted either
 * way so the new operation text appears.
 *
 * @param[in] request Request to copy into the queue.
 */
void send_request(const struct cardsight_request *request)
{
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	if (app.busy) {
		mutex_unlock(&app_lock);
		return;
	}
	app.busy = true;
	app.error = 0;
	app.progress = -1;
	app.request = request->type;
	strcpy(app.operation, "正在处理");
	mutex_unlock(&app_lock);
	if (msg_queue_put(&requests, request, NO_WAIT) != 0) {
		mutex_lock_timeout(&app_lock, WAIT_FOREVER);
		app.busy = false;
		app.error = -EBUSY;
		strcpy(app.operation, "请求队列已满");
		mutex_unlock(&app_lock);
	}
	app_post_refresh();
}


/**
 * @brief Start the lazy exFAT view and expose it over USB mass storage.
 *
 * @details The lazy engine indexes a source directory only when the host reads
 * it, so it can expose a full ext4 root without a start-up scan. The view is
 * started first; USB is only started once the block device exists. A failed
 * USB start tears the view down again.
 *
 * @param[in] selected Bit mask of source partitions to expose.
 * @return 0 on success, or the error from the proxy or the USB driver.
 */
static int start_proxy(uint32_t selected)
{
	int result;

	usb_mass_storage_force_read_only(false);
	result = storage_proxy_lazy_start(CONFIG_STORAGE_PROXY_SOURCE_DISK, selected);
	if (result == 0) {
		result = usb_mass_storage_start_device(storage_proxy_lazy_device_name());
	}
	if (result != 0) {
		storage_proxy_lazy_stop();
	}
	return result;
}


/**
 * @brief The application task: owns the card, runs requests and polls live state.
 *
 * @details Start-up scans the card once and records a failure only when a card
 * is actually present. The main loop then repeats four phases:
 * - watch the slot detection flag and the MMC probe error, so a card that is
 *   being identified or that failed identification is reflected in the UI;
 * - wait up to 200 ms for a request and run exactly one, translating its
 *   result into the operation text and error detail;
 * - notice a card insertion or removal and tear down whatever was serving the
 *   old card before rescanning;
 * - while USB mass storage runs, sample its counters once per second and
 *   compute the read and write rates.
 * All shared state is written under @c app_lock and published with a UI
 * refresh after the lock is released.
 *
 * @param[in] argument Unused task argument.
 */
void worker(void *argument)
{
	struct cardsight_request request;
	struct usb_mass_storage_stats previous_stats = { 0 };
	uint64_t previous_stats_ms = 0U;
	bool last_configured = false;
	bool last_present;
	int last_probe_error = 0;
	uint64_t probe_since_ms = 0U;

	(void)argument;
	{
		char detail[48] = "";
		int result;

		mmc_last_error_clear();
		result = card_scan_check(detail, sizeof(detail));
		if (result != 0 && card_disk_present()) {
			mutex_lock_timeout(&app_lock, WAIT_FOREVER);
			app.error = result;
			app.request = CARDSIGHT_REQUEST_SCAN;
			strcpy(app.operation, detail[0] != '\0' ? "分区无法挂载" : failure_text(result));
			snprintf(app.card_error, sizeof(app.card_error), "%s",
				detail[0] != '\0' ? detail : mmc_last_error());
			mutex_unlock(&app_lock);
		}
	}
	last_present = card_disk_present();
	/* Nothing can be posted before LVGL is up; its queue would reject the posts. */
	while (!lvgl_port_ready()) {
		msleep(20);
	}
	app_post_refresh();
	for (;;) {
		{
			bool detecting = mmc_slot_detecting();

			mutex_lock_timeout(&app_lock, WAIT_FOREVER);
			if (app.card_detecting != detecting) {
				app.card_detecting = detecting;
				mutex_unlock(&app_lock);
				app_post_refresh_runtime();
			} else {
				mutex_unlock(&app_lock);
			}
		}
		{
			int probe = mmc_probe_error();

			if (probe == 0) {
				probe_since_ms = 0U;
				if (last_probe_error != 0) {
					mutex_lock_timeout(&app_lock, WAIT_FOREVER);
					if (app.error == last_probe_error) {
						app.error = 0;
						app.card_error[0] = '\0';
					}
					mutex_unlock(&app_lock);
					last_probe_error = 0;
					app_post_refresh_final();
				}
			} else {
				uint64_t now = ktime_get_ms();

				if (probe != last_probe_error) {
					last_probe_error = probe;
					probe_since_ms = now;
				} else if (probe_since_ms != 0U && now - probe_since_ms >= 10000U &&
					app.error != probe) {
					/* A card that answers late (a power-up fallback) must not flash the
					 * sheet; ten seconds of failures is a real identification problem. */
					mutex_lock_timeout(&app_lock, WAIT_FOREVER);
					if (!app.busy) {
						app.error = probe;
						app.request = CARDSIGHT_REQUEST_SCAN;
						strcpy(app.operation, "卡识别失败");
						snprintf(app.card_error, sizeof(app.card_error), "%s",
							mmc_last_error());
						mutex_unlock(&app_lock);
						app_post_refresh_final();
					} else {
						mutex_unlock(&app_lock);
					}
				}
			}
		}
		if (msg_queue_get(&requests, &request, 200) == 0) {
			char detail[48] = "";
			int result = 0;

			mutex_lock_timeout(&app_lock, WAIT_FOREVER);
			app.busy = true;
			app.error = 0;
			app.progress = -1;
			strcpy(app.operation, request.type == CARDSIGHT_REQUEST_SCAN ? "正在刷新" :
				request.type == CARDSIGHT_REQUEST_STOP ? "正在安全停止" :
				request.type == CARDSIGHT_REQUEST_FORMAT ? "正在格式化" :
				request.type == CARDSIGHT_REQUEST_LIST ? "正在读取目录" :
				request.type == CARDSIGHT_REQUEST_LAYOUT ? "正在格式化" : "正在连接USB");
			mutex_unlock(&app_lock);
			mmc_last_error_clear();
			app_post_refresh();
			if (request.type == CARDSIGHT_REQUEST_SCAN) {
				result = card_scan_check(detail, sizeof(detail));
			} else if (request.type == CARDSIGHT_REQUEST_START_DIRECT) {
				result = usb_mass_storage_start_device(CONFIG_STORAGE_PROXY_SOURCE_DISK);
			} else if (request.type == CARDSIGHT_REQUEST_START_PROXY) {
				result = start_proxy(request.selected);
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.proxy_active = result == 0;
				mutex_unlock(&app_lock);
			} else if (request.type == CARDSIGHT_REQUEST_STOP) {
				usb_mass_storage_stop();
				storage_proxy_lazy_stop();
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.proxy_active = false;
				mutex_unlock(&app_lock);
			} else if (request.type == CARDSIGHT_REQUEST_FORMAT) {
				result = usb_mass_storage_running() ? -EBUSY : format_partition(request.partition, request.content, request.full);
			} else if (request.type == CARDSIGHT_REQUEST_LIST) {
				result = card_list_files(request.partition, request.path);
			} else if (request.type == CARDSIGHT_REQUEST_LAYOUT) {
				result = usb_mass_storage_running() ? -EBUSY : layout_apply(&request);
			}
			/* The card now holds something else: re-read its table and partitions. */
			if (request.type == CARDSIGHT_REQUEST_FORMAT || request.type == CARDSIGHT_REQUEST_LAYOUT) {
				(void)card_scan();
			}
			mutex_lock_timeout(&app_lock, WAIT_FOREVER);
			app.configured = usb_mass_storage_configured();
			app.busy = false;
			app.progress = -1;
			app.error = result;
			if (request.type == CARDSIGHT_REQUEST_FORMAT && result == -ENOSPC) {
				/* FatFs aborts a filesystem that cannot fit; the partition is too small. */
				strcpy(app.operation, "分区容量不足");
				strcpy(app.card_error, "分区太小，无法建立该文件系统");
			} else {
				strcpy(app.operation, detail[0] != '\0' ? "分区无法挂载" :
					failure_text(result));
				snprintf(app.card_error, sizeof(app.card_error), "%s",
					result != 0 ? (detail[0] != '\0' ? detail : mmc_last_error()) : "");
			}
			mutex_unlock(&app_lock);
			app_post_refresh_final();
		}
		if (card_disk_present() != last_present) {
			/* A card came or went: drop what was serving it, then look at the new state. */
			last_present = !last_present;
			if (!last_present) {
				pr_info("card removed: stopping USB and the proxy\n");
				usb_mass_storage_stop();
				pr_info("card removed: USB stopped\n");
				storage_proxy_lazy_stop();
				pr_info("card removed: proxy stopped\n");
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.proxy_active = false;
				app.files.valid = false;
				app.error = 0;
				app.card_error[0] = '\0';
				mutex_unlock(&app_lock);
				/* The info, the partitions and the capacity belong to the card that is
				 * gone; a rescan of the empty slot clears them without an error. */
				(void)card_scan();
			}
			pr_info("card %s\n", last_present ? "inserted" : "removed");
			if (last_present) {
				/* Only a card that is really there is scanned; an empty slot is not
				 * a failure and must not open the error sheet. */
				char detail[48] = "";
				int result;

				mmc_last_error_clear();
				result = card_scan_check(detail, sizeof(detail));
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.error = result;
				app.request = CARDSIGHT_REQUEST_SCAN;
				strcpy(app.operation, detail[0] != '\0' ? "分区无法挂载" : failure_text(result));
				snprintf(app.card_error, sizeof(app.card_error), "%s",
					result != 0 ? (detail[0] != '\0' ? detail : mmc_last_error()) : "");
				mutex_unlock(&app_lock);
			}
			app_post_refresh();
		}
		if (usb_mass_storage_running()) {
			bool configured = usb_mass_storage_configured();
			uint64_t now_ms = ktime_get_ms();

			if (configured != last_configured) {
				last_configured = configured;
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.configured = configured;
				mutex_unlock(&app_lock);
				app_post_refresh_runtime();
			}
			if (now_ms - previous_stats_ms >= 1000U) {
				struct usb_mass_storage_stats stats;
				uint64_t elapsed = previous_stats_ms == 0U ? 1000U : now_ms - previous_stats_ms;

				usb_mass_storage_get_stats(&stats);
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.bytes_read = stats.bytes_read;
				app.bytes_written = stats.bytes_written;
				app.read_rate = (uint32_t)((stats.bytes_read - previous_stats.bytes_read) * 1000U / elapsed);
				app.write_rate = (uint32_t)((stats.bytes_written - previous_stats.bytes_written) * 1000U / elapsed);
				mutex_unlock(&app_lock);
				previous_stats = stats;
				previous_stats_ms = now_ms;
				app_post_refresh_runtime();
			}
		} else {
			last_configured = false;
			previous_stats_ms = 0U;
			memset(&previous_stats, 0, sizeof(previous_stats));
		}
	}
}
