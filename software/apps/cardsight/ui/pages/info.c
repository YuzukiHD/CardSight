// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"
#include <stdarg.h>

#define INFO_LABEL_WIDTH 72

/* One ruled row: muted name on the left, value wrapping on the right. */
static void info_row(lv_obj_t *body, const char *name, const char *format, ...)
{
	char value[96];
	va_list arguments;
	lv_obj_t *row = make_box(body, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_t *label;

	va_start(arguments, format);
	vsnprintf(value, sizeof(value), format, arguments);
	va_end(arguments);
	lv_obj_set_style_pad_ver(row, 8, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(row, lv_color_hex(UI_OUTLINE), 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 8, 0);
	label = make_label(row, name, UI_MUTED);
	lv_obj_set_width(label, INFO_LABEL_WIDTH);
	label = make_label(row, value, UI_TEXT);
	lv_obj_set_flex_grow(label, 1);
}

static void info_card_rows(lv_obj_t *body)
{
	char capacity[24];
	char modes[64] = "default";
	char cid[33] = { 0 };
	const struct mmc_card_info *card = &ui.model.card;

	capacity_text(ui.model.sectors, capacity, sizeof(capacity));
	info_row(body, "容量", "%s", capacity);
	info_row(body, "类型", "%s", card->high_capacity ? ui.model.sectors > 67108864U ? "SDXC" : "SDHC" : "SDSC");
	info_row(body, "分区表", "%s  %u 个分区", ui.model.table == BLK_PARTITION_TABLE_GPT ? "GPT" : "MBR",
		(unsigned int)ui.model.partition_count);
	info_row(body, "访问", "%s", ui.model.card_writable ? "可读写" : "只读");
	if (ui.model.card_info_valid) {
		for (unsigned int index = 0U; index < sizeof(card->cid); index++) {
			snprintf(cid + index * 2U, sizeof(cid) - index * 2U, "%02x", card->cid[index]);
		}
		if ((card->sw_groups[0] & (1U << 1)) != 0U) strcat(modes, "/SDR25");
		if ((card->sw_groups[0] & (1U << 2)) != 0U) strcat(modes, "/SDR50");
		if ((card->sw_groups[0] & (1U << 3)) != 0U) strcat(modes, "/SDR104");
		if ((card->sw_groups[0] & (1U << 4)) != 0U) strcat(modes, "/DDR50");
		info_row(body, "厂商", "%02X  OEM %.2s", card->identity.manufacturer_id, card->identity.oem_id);
		info_row(body, "产品", "%.5s  v%u.%u", card->identity.product_name,
			card->identity.product_revision_major, card->identity.product_revision_minor);
		info_row(body, "序列", "%08X", (unsigned int)card->identity.serial_number);
		info_row(body, "制造年月", "%04u-%02u", card->identity.manufacture_year, card->identity.manufacture_month);
		info_row(body, "模式", "%s  %u MHz  %d-bit", card->mode, (unsigned int)(card->clock / 1000000U),
			card->bus_width);
		info_row(body, "1.8V", "%s", card->s18a ? "支持" : "不支持");
		info_row(body, "调谐", "%s", card->tuned ? "完成" : "无");
		info_row(body, "能力", "%s", modes);
		info_row(body, "最大电流", "%u mA", card->max_current);
		info_row(body, "CID", "%s", cid);
	}
	info_row(body, "写保护检测", "不支持");
}

static void info_partition_rows(lv_obj_t *body)
{
	for (size_t index = 0U; index < ui.model.partition_count; index++) {
		const struct storage_proxy_partition_info *part = &ui.model.partitions[index];
		char name[8];
		char size[20];

		snprintf(name, sizeof(name), "P%u", part->partno);
		capacity_text(part->sectors, size, sizeof(size));
		info_row(body, name, "%s  %s  %s  剩余%lluMB", part->status == 0 ? part->volume.fstype : "无法识别",
			size, part->writable ? "RW" : "RO",
			(unsigned long long)((part->volume.free_bytes + 500000U) / 1000000U));
	}
}

void build_info(void)
{
	lv_obj_t *body = make_list(0, 0);

	lv_obj_set_style_pad_row(body, 0, 0);
	info_card_rows(body);
	info_partition_rows(body);
}
