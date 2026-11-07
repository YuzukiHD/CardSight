// SPDX-License-Identifier: GPL-2.0-or-later
/* The application state, the worker that runs one request at a time, and what it posts to the UI. */

#include "app/app_internal.h"

/* The state every module reads, and the queue the UI sends requests through. */
struct cardsight_state app;
struct mutex app_lock;
struct message_queue requests;


static void app_post_refresh(void)
{
	(void)lvgl_port_post(cardsight_ui_refresh, NULL);
}


/* The last refresh of a request must not be lost to a full queue, or the banner never clears. */
static void app_post_refresh_final(void)
{
	for (uint32_t attempt = 0U; attempt < 100U; attempt++) {
		if (lvgl_port_post(cardsight_ui_refresh, NULL) == 0) {
			return;
		}
		msleep(10);
	}
}


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


/*
 * A card whose volumes all fail to probe is inserted but unreadable: report the first failure
 * with its device name so the sheet can say which partition it was.
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

void app_post_refresh_runtime(void)
{
	(void)lvgl_port_post(cardsight_ui_runtime, NULL);
}


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


static int start_proxy(uint32_t selected)
{
	const struct storage_proxy_config config = {
		.source_disk = CONFIG_STORAGE_PROXY_SOURCE_DISK,
		.selected_partitions = selected,
	};
	int result = storage_proxy_start_config(&config);

	if (result == 0) {
		result = usb_mass_storage_start_device(storage_proxy_device_name());
	}
	if (result != 0) {
		storage_proxy_stop();
	}
	return result;
}


static void worker(void *argument)
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
	while (!lvgl_port_ready()) {
		msleep(20);
	}
	app_post_refresh();
	for (;;) {
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
				request.type == CARDSIGHT_REQUEST_ERASE ? "正在擦除" :
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
				storage_proxy_stop();
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.proxy_active = false;
				mutex_unlock(&app_lock);
			} else if (request.type == CARDSIGHT_REQUEST_FORMAT) {
				result = usb_mass_storage_running() ? -EBUSY : format_partition(request.partition, request.content, request.full);
			} else if (request.type == CARDSIGHT_REQUEST_LIST) {
				result = card_list_files(request.partition, request.path);
			} else if (request.type == CARDSIGHT_REQUEST_LAYOUT) {
				result = usb_mass_storage_running() ? -EBUSY : layout_apply(&request);
			} else if (request.type == CARDSIGHT_REQUEST_ERASE) {
				result = usb_mass_storage_running() ? -EBUSY : erase_card();
			}
			if (request.type == CARDSIGHT_REQUEST_FORMAT || request.type == CARDSIGHT_REQUEST_LAYOUT ||
				request.type == CARDSIGHT_REQUEST_ERASE) {
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
				storage_proxy_source_lost();
				usb_mass_storage_stop();
				pr_info("card removed: USB stopped\n");
				storage_proxy_stop();
				pr_info("card removed: proxy stopped\n");
				mutex_lock_timeout(&app_lock, WAIT_FOREVER);
				app.proxy_active = false;
				app.files.valid = false;
				app.error = 0;
				app.card_error[0] = '\0';
				mutex_unlock(&app_lock);
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


void cardsight_app_init(void)
{
	memset(&app, 0, sizeof(app));
	app.progress = -1;
	mutex_init(&app_lock);
	if (app_lock.handle == NULL || msg_queue_init(&requests, sizeof(struct cardsight_request), 6U) != 0) {
		pr_err("cannot initialize CardSight storage app\n");
		return;
	}
	if (xTaskCreate(worker, "cardsight", 8192U, NULL, 2U, NULL) != pdPASS) {
		pr_err("cannot create CardSight storage task\n");
	}
}


void cardsight_snapshot(struct cardsight_state *state)
{
	mutex_lock_timeout(&app_lock, WAIT_FOREVER);
	*state = app;
	state->running = usb_mass_storage_running();
	mutex_unlock(&app_lock);
}
