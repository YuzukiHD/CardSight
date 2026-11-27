// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_shell.c
 * @brief App bar, wallpaper/backdrop and the card-detection overlay.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/**
 * @brief Seed the layout editor with a default three-partition split.
 *
 * @details Uses up to three partitions, gives each all but the last MiB of the
 *          card divided by the partition count, and assigns FAT32, exFAT, and
 *          ext4 in order. The last partition keeps a zero size, meaning "rest of
 *          the card".
 */
void default_layout(void)
{
	uint64_t available_mib = ui.model.sectors / 2048U;
	uint32_t count = ui.model.partition_count == 0U ? 3U : (uint32_t)MIN(ui.model.partition_count, 3U);

	ui.layout_count = count;
	for (uint32_t index = 0U; index < APP_LAYOUT_MAX; index++) {
		ui.layout_mib[index] = index + 1U == count ? 0U : (uint32_t)((available_mib - 1U) / count);
		ui.layout_content[index] = index == 0U ? BLK_PARTITION_FAT32 :
			index == 1U ? BLK_PARTITION_EXFAT : BLK_PARTITION_EXT4;
	}
}

/* The identification happens before the block device exists, so the pages cannot tell. */
/**
 * @brief Show or hide the card-identification spinner.
 *
 * @details Identification happens before the block device exists, so the pages
 *          cannot tell; this overlay is driven straight from the model flag.
 */
void ui_update_detecting(void)
{
	if (ui.detecting_card != NULL) {
		lv_obj_set_hidden(ui.detecting_card, !ui.model.card_detecting);
	}
}

/**
 * @brief Select the wallpaper variant for the current page.
 *
 * @details Text-heavy pages get the frosted wallpaper, while the home page keeps
 *          the sharp artwork. The list fade strips are repainted afterwards.
 */
void ui_update_backdrop(void)
{
	/* Text-heavy pages get the frosted wallpaper; the home page stays sharp. */
	if (ui.background != NULL) {
		lv_image_set_src(ui.background, ui.mode == CARDSIGHT_DIRECT ? backdrop_image() :
			backdrop_text_image());
	}
	ui_refresh_list_fades();
}

/**
 * @brief Refresh the app bar title, item count, and card status.
 *
 * @details The title is the layout editor's name or the page name; the status
 *          shows "identifying", the formatted capacity, or "no card"; the count
 *          appears only on the Files detail view.
 */
void ui_update_header(void)
{
	/** @brief Page title of each tab, indexed by cardsight_mode. */
	static const char *const names[APP_TAB_COUNT] = { "Direct", "Proxy", "Disk", "Files", "Info" };
	char title[48];
	char count[24] = "";
	char status[24];

	if (ui.header_title == NULL) {
		return;
	}
	if (ui.layout_editor) {
		snprintf(title, sizeof(title), "Format");
	} else {
		snprintf(title, sizeof(title), "%s", names[ui.mode]);
	}
	if (ui.model.card_detecting) {
		snprintf(status, sizeof(status), "Identifying");
	} else if (ui.model.card_present) {
		capacity_text(ui.model.sectors, status, sizeof(status));
	} else {
		snprintf(status, sizeof(status), "No card");
	}
	if (ui.mode == CARDSIGHT_FILES && ui.files_detail && ui.model.files.valid) {
		snprintf(count, sizeof(count), "%u items", (uint32_t)ui.model.files.total);
	}
	lv_label_set_text(ui.header_title, title);
	lv_label_set_text(ui.header_count, count);
	lv_label_set_text(ui.header_status, status);
}
