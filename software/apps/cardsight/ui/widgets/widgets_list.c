// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file widgets_list.c
 * @brief Scrolling lists and their wallpaper fade strips.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

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
