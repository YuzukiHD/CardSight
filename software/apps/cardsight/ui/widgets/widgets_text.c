// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file widgets_text.c
 * @brief Text formatting, labels and ellipsis handling.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/*
 * Capacity is quoted in the decimal units the card is sold in: a 32 GB card holds
 * 31.9e9 bytes and reads "32 GB", not "29.7 GiB".
 */
/**
 * @brief Format a sector count as a decimal capacity string.
 *
 * @details Converts to bytes and picks TB, GB, or MB by magnitude. Each unit
 *          is rounded half up in integer arithmetic, and the labels deliberately
 *          use decimal units because that is how cards are sold: a 32 GB card
 *          holds 31.9e9 bytes and reads "32 GB", not "29.7 GiB".
 *
 * @param[in]  sectors Capacity in 512-byte sectors.
 * @param[out] output  Buffer receiving the formatted text.
 * @param[in]  size    Size of @p output in bytes.
 */
void capacity_text(uint64_t sectors, char *output, size_t size)
{
	uint64_t bytes = sectors * 512U;

	if (bytes >= 1000000000000ULL) {
		snprintf(output, size, "%llu TB",
			(unsigned long long)((bytes + 500000000000ULL) / 1000000000000ULL));
	} else if (bytes >= 1000000000ULL) {
		snprintf(output, size, "%llu GB",
			(unsigned long long)((bytes + 500000000ULL) / 1000000000ULL));
	} else {
		snprintf(output, size, "%llu MB",
			(unsigned long long)((bytes + 500000ULL) / 1000000ULL));
	}
}

/**
 * @brief Return the short display name of a partition content type.
 *
 * @param[in] content Partition content type.
 * @return "FAT32", "exFAT", or "ext4"; never NULL.
 */
const char *content_name(enum blk_partition_content content)
{
	return content == BLK_PARTITION_FAT32 ? "FAT32" : content == BLK_PARTITION_EXFAT ? "exFAT" : "ext4";
}

/* Each filesystem keeps one colour on every page: FAT32 blue, exFAT green, ext4 amber. */
/**
 * @brief Return the chip colour used for a filesystem name.
 *
 * @details Sniffs the leading characters rather than parsing the name. Each
 *          filesystem keeps one colour on every page: FAT32 is the accent
 *          blue, exFAT the success green, and ext4 the warning amber; anything
 *          else falls back to the muted colour.
 *
 * @param[in] fstype Filesystem name as reported by the block layer.
 * @return Colour for the filesystem chip.
 */
uint32_t fstype_color(const char *fstype)
{
	if (fstype[0] == 'F' || fstype[0] == 'f') return UI_ACCENT;
	if (fstype[0] == 'e' && fstype[1] == 'x') return fstype[2] == 't' ? UI_WARNING : UI_SUCCESS;
	return UI_MUTED;
}

/**
 * @brief Create a themed label as a child of @p parent.
 *
 * @param[in] parent Parent object.
 * @param[in] text   Initial label text.
 * @param[in] color  Text colour, 0xRRGGBB.
 * @return The new label object.
 */
lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

/**
 * @brief Set label text, cutting it on a whole UTF-8 character with an ellipsis.
 *
 * @details Measures the text with the label's current font and letter spacing.
 *          When it fits, or when the buffer cannot hold the text plus an
 *          ellipsis, the text is set unchanged. Otherwise the string is
 *          shortened from the end one character at a time, backing off over
 *          UTF-8 continuation bytes, until the text with an ellipsis fits. A
 *          plain clipped label would cut the last glyph in half.
 *
 * @param[in,out] label     Label to update.
 * @param[in]     text      New text, possibly multi-byte UTF-8; NULL is treated
 *                          as an empty string.
 * @param[in]     max_width Available width in pixels; the text is shortened
 *                          when it exceeds this width.
 */
void ui_label_set_ellipsis(lv_obj_t *label, const char *text, int32_t max_width)
{
	/** @brief UTF-8 ellipsis appended to shortened labels. */
	static const char ellipsis[] = "…";
	char buffer[VFS_PATH_MAX + sizeof(ellipsis)];
	const lv_font_t *font;
	int32_t letter_space;
	lv_point_t size;
	size_t length;
	size_t cut;

	if (text == NULL) {
		text = "";
	}
	font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
	length = strlen(text);
	/* no width to fit into, or too long for the local buffer: take it as is */
	if (max_width <= 0 || length + sizeof(ellipsis) > sizeof(buffer)) {
		lv_label_set_text(label, text);
		return;
	}
	lv_text_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	if (size.x <= max_width) {
		lv_label_set_text(label, text);
		return;
	}
	cut = length;
	while (cut > 0U) {
		/* back off to the start of the previous UTF-8 character */
		cut--;
		while (cut > 0U && ((unsigned char)text[cut] & 0xc0U) == 0x80U) {
			cut--;
		}
		memcpy(buffer, text, cut);
		memcpy(buffer + cut, ellipsis, sizeof(ellipsis));
		lv_text_get_size(&size, buffer, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= max_width) {
			lv_label_set_text(label, buffer);
			return;
		}
	}
	/* not even one character fits: show only the ellipsis */
	lv_label_set_text(label, ellipsis);
}

/**
 * @brief Shared click handler that starts or stops the active card operation.
 *
 * @details Builds a request from the current model: STOP when an operation is
 *          already running, otherwise START_PROXY on the Proxy page and
 *          START_DIRECT everywhere else. The selected index is carried along
 *          and the request is handed to the application worker.
 *
 * @param[in] event LVGL click event; not inspected.
 */
void start_event(lv_event_t *event)
{
	struct cardsight_request request = { 0 };

	(void)event;
	request.type = ui.model.running ? CARDSIGHT_REQUEST_STOP :
		ui.mode == CARDSIGHT_PROXY ? CARDSIGHT_REQUEST_START_PROXY : CARDSIGHT_REQUEST_START_DIRECT;
	request.selected = ui.selected;
	send_request(&request);
}
