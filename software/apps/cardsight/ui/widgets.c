// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/ui_internal.h"

/*
 * Capacity is quoted in the decimal units the card is sold in: a 32 GB card holds
 * 31.9e9 bytes and reads "32 GB", not "29.7 GiB".
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

const char *content_name(enum blk_partition_content content)
{
	return content == BLK_PARTITION_FAT32 ? "FAT32" : content == BLK_PARTITION_EXFAT ? "exFAT" : "ext4";
}

/* Each filesystem keeps one colour on every page: FAT32 blue, exFAT green, ext4 amber. */
uint32_t fstype_color(const char *fstype)
{
	if (fstype[0] == 'F' || fstype[0] == 'f') return UI_ACCENT;
	if (fstype[0] == 'e' && fstype[1] == 'x') return fstype[2] == 't' ? UI_WARNING : UI_SUCCESS;
	return UI_MUTED;
}

void disable_scroll(lv_obj_t *object)
{
	lv_obj_set_scrollable(object, false);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

void enable_vertical_scroll(lv_obj_t *object)
{
	lv_obj_set_scrollable(object, true);
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	/* the reference UI has no visible scrollbars; the list still drags and scrolls */
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

void ui_label_set_ellipsis(lv_obj_t *label, const char *text, int32_t max_width)
{
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
	lv_label_set_text(label, ellipsis);
}

/* Transparent, inert layout container. */
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
int32_t ui_margin(void)
{
	return (ui.screen_radius * 3 + 5) / 10 + 1;
}

/* Corner radius of anything that sits APP_BAR_MARGIN from the edge, concentric with the glass. */
int32_t ui_bar_radius(void)
{
	return MIN(MAX(ui.screen_radius - APP_BAR_MARGIN, 8), APP_BAR_HEIGHT / 2);
}

/* Width of controls that sit clear of the corners, such as the docked action button. */
int32_t ui_wide_width(void)
{
	return (int32_t)ui.width - 2 * APP_EDGE_PAD;
}

int32_t ui_page_width(void)
{
	return (int32_t)ui.width - 2 * ui_margin();
}

/* Content stops above the bottom bar, which always holds the tabs or the back button. */
int32_t ui_page_height(void)
{
	return (int32_t)ui.height - ui_margin() - APP_HEADER_HEIGHT - APP_BAR_MARGIN - APP_BAR_HEIGHT -
		APP_GAP;
}

/* Button that fills a bottom bar, with the corners following the glass. */
static void press_layer_anim_opa(void *object, int32_t value)
{
	lv_obj_set_style_bg_opa(object, (lv_opa_t)value, 0);
}

/* Fade the white layer in and out instead of switching it, so the press reads as a glow. */
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
#define LIST_FADE_COUNT 6U
#define LIST_FADE_HEIGHT 16

#define FADE_GUARD 0x5a17c0deU

struct list_fade {
	lv_obj_t *tile;
	lv_obj_t *list;
	lv_obj_t *top_image;
	lv_obj_t *bottom_image;
	lv_image_dsc_t top_dsc;
	lv_image_dsc_t bottom_dsc;
	uint8_t *top_pixels;
	uint8_t *bottom_pixels;
	uint8_t *top_alloc;
	uint8_t *bottom_alloc;
	int32_t x;
	int32_t y;
	int32_t origin_x; /* the tile's screen offset: the strips sample the wallpaper in screen coords */
	int32_t origin_y;
	int32_t width;
	int32_t height;
};

static struct list_fade list_fades[LIST_FADE_COUNT];

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
static void list_fade_update(struct list_fade *fade)
{
	lv_obj_set_hidden(fade->top_image, lv_obj_get_scroll_top(fade->list) <= 0);
	lv_obj_set_hidden(fade->bottom_image, lv_obj_get_scroll_bottom(fade->list) <= 0);
}

static void list_fade_event(lv_event_t *event)
{
	list_fade_update(lv_event_get_user_data(event));
}

/* The strip pixels are written by the renderer and read by the image draw code; a guard word on
 * each end catches the usual off-by-a-row overrun without instrumenting every write. */
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

void set_disabled(lv_obj_t *object, bool disabled)
{
	if (disabled) lv_obj_add_state(object, LV_STATE_DISABLED);
	else lv_obj_remove_state(object, LV_STATE_DISABLED);
}
