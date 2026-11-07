// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file widgets.c
 * @brief Widget constructors, list fades, and geometry helpers for the UI.
 *
 * Every page composes its tree from the small builders here: labels, boxes,
 * cards, glass sheets, chips, buttons, and the vertically scrolling lists with
 * their wallpaper fade strips.
 */
#include "ui/ui_internal.h"

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
 * @brief Disable both scroll directions on an object.
 *
 * @param[in,out] object Object to make static.
 */
void disable_scroll(lv_obj_t *object)
{
	lv_obj_set_scrollable(object, false);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

/**
 * @brief Enable vertical scrolling with the themed elastic behaviour.
 *
 * @details Restricts scrolling to the vertical direction and hides the
 *          scrollbar: the reference UI has no visible scrollbars, though the
 *          list still drags and scrolls.
 *
 * @param[in,out] object Object that should scroll vertically.
 */
void enable_vertical_scroll(lv_obj_t *object)
{
	lv_obj_set_scrollable(object, true);
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	/* the reference UI has no visible scrollbars; the list still drags and scrolls */
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
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

/* Transparent, inert layout container. */
/**
 * @brief Create a transparent, inert layout container.
 *
 * @details Transparent background, no border or padding, not clickable, and
 *          no scrolling; it exists only to group and position children.
 *
 * @param[in] parent Parent object.
 * @param[in] width  Requested width in pixels.
 * @param[in] height Requested height in pixels.
 * @return The new box object.
 */
lv_obj_t *make_box(lv_obj_t *parent, int32_t width, int32_t height)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_set_size(box, width, height);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_clickable(box, false);
	disable_scroll(box);
	return box;
}

/**
 * @brief Create a styled card as a child of @p parent.
 *
 * @details Builds a box and applies the surface colour and the standard card
 *          radius.
 *
 * @param[in] parent Parent object.
 * @param[in] width  Requested width in pixels.
 * @param[in] height Requested height in pixels.
 * @param[in] color  Background colour, 0xRRGGBB.
 * @return The new card object.
 */
lv_obj_t *make_card(lv_obj_t *parent, int32_t width, int32_t height, uint32_t color)
{
	lv_obj_t *card = make_box(parent, width, height);

	ui_style_card(card, color, APP_CARD_RADIUS);
	return card;
}

/*
 * A liquid-glass surface: its own refraction canvas (see glass.c), a one-pixel light edge and a
 * soft shadow. The canvas is the object's background image, so it moves and clips with it.
 */
/**
 * @brief Create a lifted glass sheet with a light edge, specular rim and soft shadow.
 *
 * @details Builds a transparent rounded object with corner clipping, a
 *          one-pixel light border, and a soft black shadow below. The
 *          refraction canvas from glass.c is attached as the object's
 *          background image, so it moves and clips with the sheet.
 *
 * @param[in] parent Parent object.
 * @param[in] x      Left edge in the parent's coordinate space.
 * @param[in] y      Top edge in the parent's coordinate space.
 * @param[in] width  Width in pixels.
 * @param[in] height Height in pixels.
 * @param[in] radius Corner radius in pixels.
 * @return The new glass sheet object.
 */
lv_obj_t *make_glass_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
	int32_t radius)
{
	lv_obj_t *card = lv_obj_create(parent);

	lv_obj_set_size(card, width, height);
	lv_obj_set_pos(card, x, y);
	lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_color(card, lv_color_hex(UI_GLASS_EDGE), 0);
	lv_obj_set_style_border_opa(card, UI_GLASS_EDGE_OPA, 0);
	lv_obj_set_style_radius(card, radius, 0);
	lv_obj_set_style_clip_corner(card, true, 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 10, 0);
	lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
	lv_obj_set_style_shadow_opa(card, 70, 0);
	lv_obj_set_style_shadow_ofs_y(card, 3, 0);
	lv_obj_set_clickable(card, false);
	disable_scroll(card);
	glass_attach(card, radius);
	return card;
}

/* Small rounded tag whose background is the text colour tinted into the surface. */
/**
 * @brief Create a small rounded status tag.
 *
 * @details The tag is a content-sized, 24-pixel-high box holding a coloured
 *          label, with a row flex layout that centres the text. Its background
 *          is the text colour tinted into the surface, with the standard corner
 *          radius of 12 pixels.
 *
 * @param[in] parent Parent object.
 * @param[in] text   Chip text.
 * @param[in] color  Chip accent colour, also used for the label.
 * @return The new chip object.
 */
lv_obj_t *make_chip(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *chip = make_box(parent, LV_SIZE_CONTENT, 24);
	lv_obj_t *label = make_label(chip, text, color);

	ui_style_card(chip, UI_SURFACE, 12);
	lv_obj_set_style_bg_color(chip, ui_tint(color, 40), 0);
	lv_obj_set_style_pad_hor(chip, 7, 0);
	lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	(void)label;
	return chip;
}

/**
 * @brief Create a neutral action button with a centred clipped label.
 *
 * @details The button gets the standard height, the normal button style, and
 *          the card radius. Its label is 94 % of the button width, clipped
 *          rather than wrapped, and centred. When @p callback is non-NULL it is
 *          registered for LV_EVENT_CLICKED with @p data as user data.
 *
 * @param[in] parent   Parent object.
 * @param[in] text     Button label.
 * @param[in] callback Handler invoked on LV_EVENT_CLICKED; may be NULL.
 * @param[in] data     User data passed to @p callback.
 * @return The new button object.
 */
lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_t *label = lv_label_create(button);

	lv_obj_set_height(button, APP_BUTTON_HEIGHT);
	ui_style_button(button, UI_BUTTON_NORMAL);
	lv_obj_set_style_radius(button, APP_CARD_RADIUS, 0);
	disable_scroll(button);
	lv_label_set_text(label, text);
	lv_obj_set_width(label, lv_pct(94));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(label);
	if (callback != NULL) {
		lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
	}
	return button;
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

/* Smallest edge inset that keeps a square content corner inside the rounded glass. */
/**
 * @brief Return the smallest edge inset that keeps content inside the rounded glass.
 *
 * @details Scales the screen radius by roughly 0.3 and adds one pixel.
 *
 * @return Margin in pixels.
 */
int32_t ui_margin(void)
{
	return (ui.screen_radius * 3 + 5) / 10 + 1;
}

/* Corner radius of anything that sits APP_BAR_MARGIN from the edge, concentric with the glass. */
/**
 * @brief Return the corner radius for objects inset by APP_BAR_MARGIN.
 *
 * @details Subtracts the bar margin from the screen radius so the outline stays
 *          concentric with the glass, clamps the result to at least 8 and to
 *          half the bar height.
 *
 * @return Radius in pixels.
 */
int32_t ui_bar_radius(void)
{
	return MIN(MAX(ui.screen_radius - APP_BAR_MARGIN, 8), APP_BAR_HEIGHT / 2);
}

/* Width of controls that sit clear of the corners, such as the docked action button. */
/**
 * @brief Return the width of controls that sit clear of the corners.
 *
 * @return Screen width minus twice the edge padding, in pixels.
 */
int32_t ui_wide_width(void)
{
	return (int32_t)ui.width - 2 * APP_EDGE_PAD;
}

/**
 * @brief Return the usable content width of the current page.
 *
 * @return Screen width minus the margins on both sides, in pixels.
 */
int32_t ui_page_width(void)
{
	return (int32_t)ui.width - 2 * ui_margin();
}

/* Content stops above the bottom bar, which always holds the tabs or the back button. */
/**
 * @brief Return the usable content height of the current page.
 *
 * @details Subtracts the margins, page header, app bar, and the gap that
 *          reserves the bottom bar, which always holds the tabs or the back
 *          button.
 *
 * @return Height in pixels.
 */
int32_t ui_page_height(void)
{
	return (int32_t)ui.height - ui_margin() - APP_HEADER_HEIGHT - APP_BAR_MARGIN - APP_BAR_HEIGHT -
		APP_GAP;
}

/* Button that fills a bottom bar, with the corners following the glass. */
/**
 * @brief Animation callback that sets a layer's background opacity.
 *
 * @param[in,out] object Layer being animated.
 * @param[in]     value  New opacity, 0..255.
 */
static void press_layer_anim_opa(void *object, int32_t value)
{
	lv_obj_set_style_bg_opa(object, (lv_opa_t)value, 0);
}

/* Fade the white layer in and out instead of switching it, so the press reads as a glow. */
/**
 * @brief Fade the press layer to a new opacity instead of switching it.
 *
 * @details Starts a one-shot LVGL animation from the current opacity to
 *          @p target with an ease-out path, so the press reads as a glow.
 *
 * @param[in,out] layer    Layer to fade.
 * @param[in]     target   Target background opacity, 0..255.
 * @param[in]     duration Animation duration in milliseconds.
 */
static void press_layer_fade(lv_obj_t *layer, uint8_t target, uint32_t duration)
{
	lv_anim_t animation;

	lv_anim_init(&animation);
	lv_anim_set_var(&animation, layer);
	lv_anim_set_exec_cb(&animation, press_layer_anim_opa);
	lv_anim_set_values(&animation, lv_obj_get_style_bg_opa(layer, 0), target);
	lv_anim_set_duration(&animation, duration);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

/**
 * @brief Press/release handler that brightens or restores the press layer.
 *
 * @details On LV_EVENT_PRESSED the layer fades to opacity 52; on release or
 *          press-lost it fades back to the base opacity stored in the layer's
 *          user data. When animations are disabled the opacity is set directly
 *          instead.
 *
 * @param[in] event LVGL pressed, released, or press-lost event; the layer is
 *                  the user data.
 */
static void press_layer_event(lv_event_t *event)
{
	lv_obj_t *layer = lv_event_get_user_data(event);
	bool pressed = lv_event_get_code(event) == LV_EVENT_PRESSED;
	uint8_t base = (uint8_t)(uintptr_t)lv_obj_get_user_data(layer);

	if (ui.animations) {
		press_layer_fade(layer, pressed ? 52 : base, pressed ? 70 : 150);
	} else {
		lv_obj_set_style_bg_opa(layer, pressed ? 52 : base, 0);
	}
}

/* A rounded white layer that brightens the glass on press instead of a rectangular fill. */
/**
 * @brief Add a rounded white press-glow layer inside a button.
 *
 * @details Creates a non-clickable, one-pixel-inset white layer with the
 *          button's radius minus one. The base opacity is stored in the layer's
 *          user data for press_layer_event(), and the layer is wired to the
 *          button's pressed, released, and press-lost events. The label is
 *          raised above the layer when one is supplied.
 *
 * @param[in,out] button   Button owning the layer.
 * @param[in]     label    Button label to keep on top; may be NULL.
 * @param[in]     radius   Button corner radius in pixels.
 * @param[in]     base_opa Resting opacity of the white layer, 0..255.
 */
static void add_press_layer(lv_obj_t *button, lv_obj_t *label, int32_t radius, uint8_t base_opa)
{
	int32_t width = lv_obj_get_width(button);
	int32_t height = lv_obj_get_height(button);
	lv_obj_t *layer = lv_obj_create(button);

	lv_obj_set_size(layer, width - 2, height - 2);
	lv_obj_set_pos(layer, 1, 1);
	lv_obj_set_style_bg_color(layer, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_bg_opa(layer, base_opa, 0);
	lv_obj_set_style_border_width(layer, 0, 0);
	lv_obj_set_style_radius(layer, radius > 2 ? radius - 1 : radius, 0);
	lv_obj_set_style_pad_all(layer, 0, 0);
	lv_obj_set_clickable(layer, false);
	lv_obj_set_scrollable(layer, false);
	lv_obj_set_user_data(layer, (void *)(uintptr_t)base_opa);
	lv_obj_add_event_cb(button, press_layer_event, LV_EVENT_PRESSED, layer);
	lv_obj_add_event_cb(button, press_layer_event, LV_EVENT_RELEASED, layer);
	lv_obj_add_event_cb(button, press_layer_event, LV_EVENT_PRESS_LOST, layer);
	if (label != NULL) {
		lv_obj_move_foreground(label);
	}
}

/**
 * @brief Apply the neutral glass pill look used by the bottom bar buttons.
 *
 * @details Switches the button to the ghost style, forces the text to the
 *          primary text colour on both button and label, attaches a glass
 *          surface at the button's radius with the control tint opacity, and
 *          adds a subtle press glow.
 *
 * @param[in,out] button Button to restyle.
 */
void ui_glass_neutral(lv_obj_t *button)
{
	lv_obj_t *label = lv_obj_get_child(button, 0);
	int32_t radius = lv_obj_get_style_radius(button, 0);

	ui_style_button(button, UI_BUTTON_GHOST);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_TEXT), 0);
	if (label != NULL) {
		lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
	}
	glass_attach_tinted(button, radius, UI_GLASS_TINT_OPA);
	add_press_layer(button, label, radius, 20);
}

/**
 * @brief Turn an existing action button into a glass pill with a coloured rim.
 *
 * @details Switches to the ghost style, attaches the frosted highlight sheet,
 *          and re-enables clicking because the highlight helper makes its object
 *          click-through. A 38-opacity accent wash fills the pill, and the rim,
 *          shadow, and text take the accent colour. A press layer with zero base
 *          opacity supplies the glow.
 *
 * @param[in,out] button Button to restyle.
 * @param[in]     color  Accent colour of the rim, shadow, and wash.
 */
void ui_glass_action(lv_obj_t *button, uint32_t color)
{
	lv_obj_t *label = lv_obj_get_child(button, 0);
	int32_t width = lv_obj_get_width(button);
	int32_t height = lv_obj_get_height(button);
	int32_t radius = lv_obj_get_style_radius(button, 0);
	lv_obj_t *accent;

	ui_style_button(button, UI_BUTTON_GHOST);
	glass_highlight_attach(button, radius);
	/* the highlight helper makes its object click-through for the tab indicator; a button
	 * still has to take its presses */
	lv_obj_set_clickable(button, true);
	/* a soft accent wash turns the neutral glass into a primary action */
	accent = lv_obj_create(button);
	lv_obj_set_size(accent, width - 2, height - 2);
	lv_obj_set_pos(accent, 1, 1);
	lv_obj_set_style_bg_color(accent, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(accent, 38, 0);
	lv_obj_set_style_border_width(accent, 0, 0);
	lv_obj_set_style_radius(accent, radius > 2 ? radius - 1 : radius, 0);
	lv_obj_set_style_pad_all(accent, 0, 0);
	lv_obj_set_clickable(accent, false);
	lv_obj_set_scrollable(accent, false);
	if (label != NULL) {
		lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
		lv_obj_move_foreground(label);
	}
	lv_obj_set_style_text_color(button, lv_color_hex(UI_TEXT), 0);
	lv_obj_set_style_border_width(button, 1, 0);
	lv_obj_set_style_border_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_border_opa(button, 160, 0);
	lv_obj_set_style_shadow_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_shadow_opa(button, 60, 0);
	add_press_layer(button, label, radius, 0);
}

/**
 * @brief Create a glass button sized for the bottom action bar.
 *
 * @details Builds a normal button, resizes it to the bar height, gives it the
 *          bar radius, and applies the neutral glass treatment.
 *
 * @param[in] parent   Parent object (the bottom bar).
 * @param[in] text     Button label.
 * @param[in] callback Handler invoked on LV_EVENT_CLICKED.
 * @param[in] width    Button width in pixels.
 * @return The new button object.
 */
lv_obj_t *make_bar_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, int32_t width)
{
	lv_obj_t *button = make_button(parent, text, callback, NULL);

	lv_obj_set_size(button, width, APP_BAR_HEIGHT);
	lv_obj_set_style_radius(button, ui_bar_radius(), 0);
	ui_glass_neutral(button);
	return button;
}

/*
 * Vertical scroller inside the tile. It spans the full screen width and, when nothing is docked
 * above or below, runs to the screen edge, so rows leave through the rounded glass instead of
 * being cut off along an invisible line. `top` and `bottom` reserve room for docked controls.
 */
/* Top and bottom fades: strips cut from the wallpaper with an alpha ramp, drawn above the
 * list so scrolling text dissolves into the artwork instead of being clipped. */
/** @brief Number of fade-strip slots, one per page that can show a list. */
#define LIST_FADE_COUNT 6U
/** @brief Height of each fade strip in pixels. */
#define LIST_FADE_HEIGHT 16

/** @brief Guard word written at both ends of each fade pixel buffer. */
#define FADE_GUARD 0x5a17c0deU

/**
 * @brief One pair of wallpaper fade strips over a scrolling list.
 */
struct list_fade {
	/** @brief Page tile that owns the strips. */
	lv_obj_t *tile;
	/** @brief List whose scroll position drives the strips. */
	lv_obj_t *list;
	/** @brief Image at the top of the list. */
	lv_obj_t *top_image;
	/** @brief Image at the bottom of the list. */
	lv_obj_t *bottom_image;
	/** @brief Descriptor of the top strip image. */
	lv_image_dsc_t top_dsc;
	/** @brief Descriptor of the bottom strip image. */
	lv_image_dsc_t bottom_dsc;
	/** @brief ARGB8888 pixels of the top strip. */
	uint8_t *top_pixels;
	/** @brief ARGB8888 pixels of the bottom strip. */
	uint8_t *bottom_pixels;
	/** @brief Allocation holding the leading guard word and the top pixels. */
	uint8_t *top_alloc;
	/** @brief Allocation holding the leading guard word and the bottom pixels. */
	uint8_t *bottom_alloc;
	/** @brief Strip X position inside the tile. */
	int32_t x;
	/** @brief Strip Y position inside the tile. */
	int32_t y;
	/** @brief The tile's screen offset: the strips sample the wallpaper in screen coords. */
	int32_t origin_x; /* the tile's screen offset: the strips sample the wallpaper in screen coords */
	/** @brief The tile's screen Y offset. */
	int32_t origin_y;
	/** @brief Strip width in pixels. */
	int32_t width;
	/** @brief List height in pixels. */
	int32_t height;
};

/** @brief Fade-strip bookkeeping for the pages currently showing a list. */
static struct list_fade list_fades[LIST_FADE_COUNT];

/**
 * @brief Forget the fade strips of the previous page.
 *
 * @details The strip images are already gone with the screen, so only the
 *          pixel buffers are freed here. Leaving them behind would leak a
 *          screenful of C heap on every rebuild. The table is then cleared.
 */
void ui_list_fades_reset(void)
{
	/* The strip images are already gone with the screen; the pixel buffers are ours to free.
	 * Leaving them behind leaks a screenful of C heap on every rebuild. */
	for (uint32_t index = 0U; index < LIST_FADE_COUNT; index++) {
		lv_free(list_fades[index].top_alloc);
		lv_free(list_fades[index].bottom_alloc);
	}
	memset(list_fades, 0, sizeof(list_fades));
}

/**
 * @brief Paint both fade strips from the wallpaper.
 *
 * @details Uses the sharp artwork on the Direct page and the pre-darkened
 *          composite on text pages. For every column it samples the wallpaper
 *          at the strip's screen position, replicating the border rows when the
 *          sample falls outside the artwork, and writes an ARGB8888 pixel whose
 *          alpha ramps from opaque at the outer edge to transparent inwards.
 *
 * @param[in,out] fade Strip pair to paint.
 */
static void list_fade_paint(struct list_fade *fade)
{
	const lv_image_dsc_t *backdrop = ui.mode == CARDSIGHT_DIRECT ? backdrop_image() :
		backdrop_text_image();
	const uint8_t *source = backdrop->data;
	int source_width = (int)backdrop->header.w;
	int source_height = (int)backdrop->header.h;

	for (int32_t row = 0; row < LIST_FADE_HEIGHT; row++) {
		/* the top strip is opaque at the very top and clears downwards, the bottom one
		 * the other way round */
		uint8_t alpha_top = (uint8_t)(255 - row * 255 / (LIST_FADE_HEIGHT - 1));
		uint8_t alpha_bottom = (uint8_t)(row * 255 / (LIST_FADE_HEIGHT - 1));

		for (int32_t column = 0; column < fade->width; column++) {
			int screen_x = fade->origin_x + fade->x + column;
			int screen_y_top = fade->origin_y + fade->y + row;
			int screen_y_bottom = fade->origin_y + fade->y + fade->height -
				LIST_FADE_HEIGHT + row;
			size_t top = ((size_t)row * (size_t)fade->width + (size_t)column) * 4U;
			const uint8_t *pixel_top;
			const uint8_t *pixel_bottom;

			if (screen_x < 0 || screen_x >= source_width) {
				continue;
			}
			/* clamp the vertical samples to the artwork's last row */
			screen_y_top = screen_y_top < 0 ? 0 : screen_y_top >= source_height ? source_height - 1 : screen_y_top;
			screen_y_bottom = screen_y_bottom < 0 ? 0 : screen_y_bottom >= source_height ? source_height - 1 : screen_y_bottom;
			pixel_top = &source[((size_t)screen_y_top * source_width + screen_x) * 3U];
			pixel_bottom = &source[((size_t)screen_y_bottom * source_width + screen_x) * 3U];
			fade->top_pixels[top + 0U] = pixel_top[0];
			fade->top_pixels[top + 1U] = pixel_top[1];
			fade->top_pixels[top + 2U] = pixel_top[2];
			fade->top_pixels[top + 3U] = alpha_top;
			fade->bottom_pixels[top + 0U] = pixel_bottom[0];
			fade->bottom_pixels[top + 1U] = pixel_bottom[1];
			fade->bottom_pixels[top + 2U] = pixel_bottom[2];
			fade->bottom_pixels[top + 3U] = alpha_bottom;
		}
	}
}

/* A strip means "more content this way"; hide it when the list edge is reached. */
/**
 * @brief Show each strip only while the list has more content in that direction.
 *
 * @param[in,out] fade Strip pair to update.
 */
static void list_fade_update(struct list_fade *fade)
{
	lv_obj_set_hidden(fade->top_image, lv_obj_get_scroll_top(fade->list) <= 0);
	lv_obj_set_hidden(fade->bottom_image, lv_obj_get_scroll_bottom(fade->list) <= 0);
}

/**
 * @brief LV_EVENT_SCROLL handler that refreshes a strip pair.
 *
 * @param[in] event Scroll event carrying the strip pair as user data.
 */
static void list_fade_event(lv_event_t *event)
{
	list_fade_update(lv_event_get_user_data(event));
}

/* The strip pixels are written by the renderer and read by the image draw code; a guard word on
 * each end catches the usual off-by-a-row overrun without instrumenting every write. */
/**
 * @brief Check that no fade strip has written past its pixel buffer.
 *
 * @details Verifies the guard word at the start and end of every active strip
 *          buffer, logs a message naming the tile when one is broken, and counts
 *          the failures.
 *
 * @return Number of strips with a broken guard; zero means all buffers are
 *         intact.
 */
int ui_list_fades_check(void)
{
	int errors = 0;

	for (uint32_t index = 0U; index < LIST_FADE_COUNT; index++) {
		struct list_fade *fade = &list_fades[index];
		size_t bytes;

		if (fade->tile == NULL) {
			continue;
		}
		bytes = (size_t)fade->width * LIST_FADE_HEIGHT * 4U;
		if (*(uint32_t *)fade->top_alloc != FADE_GUARD ||
			*(uint32_t *)(fade->top_pixels + bytes) != FADE_GUARD ||
			*(uint32_t *)fade->bottom_alloc != FADE_GUARD ||
			*(uint32_t *)(fade->bottom_pixels + bytes) != FADE_GUARD) {
			pr_err("fade guard broken on %p\n", (void *)fade->tile);
			errors++;
		}
	}
	return errors;
}

/**
 * @brief Repaint and re-evaluate every active fade strip pair.
 *
 * @details Called after a layout change or rebuild so the strips follow the
 *          wallpaper under the new geometry; each strip pair is painted,
 *          visibility refreshed, and invalidated.
 */
void ui_refresh_list_fades(void)
{
	for (uint32_t index = 0U; index < LIST_FADE_COUNT; index++) {
		if (list_fades[index].tile != NULL) {
			list_fade_paint(&list_fades[index]);
			list_fade_update(&list_fades[index]);
			lv_obj_invalidate(list_fades[index].top_image);
			lv_obj_invalidate(list_fades[index].bottom_image);
		}
	}
}

/**
 * @brief Attach a fade pair to a list inside a tile.
 *
 * @details Skips lists too short for two strips plus margin, and lists that
 *          already have a strip pair. Otherwise claims a slot, allocates both
 *          ARGB8888 buffers with a guard word before and after the pixels,
 *          records the tile's content-origin offset, registers the scroll
 *          handler, and creates the two image objects at the top and bottom of
 *          the list. The strips are painted immediately.
 *
 * @param[in] tile   Page tile that owns the strips.
 * @param[in] list   List whose scrolling drives the strips.
 * @param[in] x      Strip X position inside the tile.
 * @param[in] y      List Y position inside the tile.
 * @param[in] width  Strip width in pixels.
 * @param[in] height List height in pixels.
 */
static void list_fade_attach(lv_obj_t *tile, lv_obj_t *list, int32_t x, int32_t y, int32_t width,
	int32_t height)
{
	size_t bytes = (size_t)width * LIST_FADE_HEIGHT * 4U;
	lv_area_t tile_area;
	struct list_fade *fade = NULL;

	if (height <= 2 * LIST_FADE_HEIGHT + 8) {
		return;
	}
	for (uint32_t index = 0U; index < LIST_FADE_COUNT; index++) {
		if (list_fades[index].tile == tile) {
			return; /* one pair per page is enough */
		}
		if (list_fades[index].tile == NULL && fade == NULL) {
			fade = &list_fades[index];
		}
	}
	if (fade == NULL) {
		return;
	}
	memset(fade, 0, sizeof(*fade));
	/* two guard words per buffer: one before the pixels, one after them */
	fade->top_alloc = lv_malloc(bytes + 2U * sizeof(uint32_t));
	fade->bottom_alloc = lv_malloc(bytes + 2U * sizeof(uint32_t));
	if (fade->top_alloc == NULL || fade->bottom_alloc == NULL) {
		lv_free(fade->top_alloc);
		lv_free(fade->bottom_alloc);
		fade->top_alloc = NULL;
		fade->bottom_alloc = NULL;
		return;
	}
	fade->top_pixels = fade->top_alloc + sizeof(uint32_t);
	fade->bottom_pixels = fade->bottom_alloc + sizeof(uint32_t);
	*(uint32_t *)fade->top_alloc = FADE_GUARD;
	*(uint32_t *)fade->bottom_alloc = FADE_GUARD;
	*(uint32_t *)(fade->top_pixels + bytes) = FADE_GUARD;
	*(uint32_t *)(fade->bottom_pixels + bytes) = FADE_GUARD;
	lv_obj_get_coords(tile, &tile_area);
	fade->tile = tile;
	fade->list = list;
	lv_obj_add_event_cb(list, list_fade_event, LV_EVENT_SCROLL, fade);
	/* children are positioned inside the parent's content area, not its border box */
	fade->origin_x = tile_area.x1 + lv_obj_get_style_pad_left(tile, LV_PART_MAIN);
	fade->origin_y = tile_area.y1 + lv_obj_get_style_pad_top(tile, LV_PART_MAIN);
	fade->x = x;
	fade->y = y;
	fade->width = width;
	fade->height = height;
	fade->top_dsc = (lv_image_dsc_t){
		.header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
			.w = (uint32_t)width, .h = LIST_FADE_HEIGHT,
			.stride = (uint32_t)width * 4U },
		.data_size = (uint32_t)bytes, .data = fade->top_pixels };
	fade->bottom_dsc = fade->top_dsc;
	fade->bottom_dsc.data = fade->bottom_pixels;
	/* the strips sit above the list and never take input */
	fade->top_image = lv_image_create(tile);
	lv_image_set_src(fade->top_image, &fade->top_dsc);
	lv_obj_set_pos(fade->top_image, x, y);
	lv_obj_set_clickable(fade->top_image, false);
	fade->bottom_image = lv_image_create(tile);
	lv_image_set_src(fade->bottom_image, &fade->bottom_dsc);
	lv_obj_set_pos(fade->bottom_image, x, y + height - LIST_FADE_HEIGHT);
	lv_obj_set_clickable(fade->bottom_image, false);
	list_fade_paint(fade);
}

/**
 * @brief Create the vertically scrolling list of the current page.
 *
 * @details The list spans the full screen width, starting at @p top inside the
 *          page, and reserves @p bottom pixels for docked controls. A zero
 *          @p bottom keeps one extra gap so the last row and the scrollbar stay
 *          clear of the bottom bar. The list is transparent, uses a column flex
 *          flow with six-pixel row spacing and the edge padding, and scrolls
 *          vertically without a visible scrollbar; the thumb is inset below the
 *          rounded corner. Focus outlines are cleared. A wallpaper fade pair is
 *          attached over the list.
 *
 * @param[in] top    Distance from the page top in pixels.
 * @param[in] bottom Distance reserved below the list in pixels.
 * @return The new list object.
 */
lv_obj_t *make_list(int32_t top, int32_t bottom)
{
	lv_obj_t *list = lv_obj_create(ui.content);
	int32_t margin = ui_margin();
	int32_t extra_bottom = bottom == 0 ? APP_GAP : 0;

	/*
	 * The list starts below the page header now; the extra bottom keeps its last row and the
	 * scrollbar clear of the bottom bar.
	 */
	lv_obj_set_size(list, ui.width, ui_page_height() - top - bottom - extra_bottom);
	lv_obj_set_pos(list, -margin, top);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_hor(list, APP_EDGE_PAD, 0);
	lv_obj_set_style_pad_bottom(list, extra_bottom, 0);
	lv_obj_set_style_pad_row(list, 6, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	enable_vertical_scroll(list);
	/* The thumb sits on the screen edge, starting below the rounded corner. */
	lv_obj_set_style_pad_top(list, ui.screen_radius, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_bottom(list, 4, LV_PART_SCROLLBAR);
	lv_obj_set_style_outline_width(list, 0, 0);
	lv_obj_set_style_outline_width(list, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(list, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_click_focusable(list, false);
	list_fade_attach(ui.content, list, -margin, top, (int32_t)ui.width,
		ui_page_height() - top - bottom - extra_bottom);
	return list;
}

/**
 * @brief Enable or disable a widget.
 *
 * @param[in,out] object   Widget to toggle.
 * @param[in]     disabled True to add LV_STATE_DISABLED, false to remove it.
 */
void set_disabled(lv_obj_t *object, bool disabled)
{
	if (disabled) lv_obj_add_state(object, LV_STATE_DISABLED);
	else lv_obj_remove_state(object, LV_STATE_DISABLED);
}
