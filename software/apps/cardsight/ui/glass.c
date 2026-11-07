// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file glass.c
 * @brief Glass material renderer for the CardSight UI.
 *
 * Follows the AI Passport glass system: a translucent tinted sheet over the
 * artwork, three one-pixel edge rings mixed with the wallpaper above and below,
 * and a bottom refraction segment. Everything is RGB888. Each glass surface
 * owns an LVGL image whose pixels are rendered once from the blurred backdrop
 * when the surface is attached.
 */

#include "ui/glass.h"

#include "FreeRTOS.h"
#include <bits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ui/backdrop.h"
#include "ui/theme.h"

/** @brief Maximum number of simultaneous glass surfaces. */
#define GLASS_MAX_SURFACES   20U
/** @brief Reference opacity used when scaling the rim strengths. */
#define GLASS_EDGE_STRENGTH  126U
/** @brief Tint opacity used when no explicit strength is given. */
#define GLASS_DEFAULT_OPACITY 154U
/** @brief Bytes per pixel of the rendered RGB888 surfaces. */
#define GLASS_BPP	     3U

/**
 * @brief Bookkeeping for one attached glass surface.
 */
struct glass_surface {
	/** @brief Widget the surface is attached to; NULL once the widget is gone. */
	lv_obj_t *object;
	/** @brief Image descriptor installed as the widget background. */
	lv_image_dsc_t image;
	/** @brief Heap-allocated RGB888 pixel buffer of the rendered surface. */
	uint8_t *pixels;
	/** @brief Surface width in pixels. */
	int32_t width;
	/** @brief Surface height in pixels. */
	int32_t height;
	/** @brief Corner radius in pixels. */
	int32_t radius;
	/** @brief Tint strength, 0..255. */
	uint8_t opacity;
	/** @brief True while this slot owns a rendered surface. */
	bool used;
};

/** @brief Fixed pool of glass surface slots, all released on widget deletion. */
static struct glass_surface surfaces[GLASS_MAX_SURFACES];

/* LVGL RGB888 keeps B, G, R in memory. */
/**
 * @brief Sample one pixel from an RGB888 buffer with coordinates clamped to the frame.
 *
 * @details Fractional coordinates are truncated to the containing pixel and
 *          clamped to the buffer edges, so sampling just outside the artwork
 *          repeats the border. LVGL RGB888 keeps B, G, R in memory, so the
 *          channels are read in that order and returned by name.
 *
 * @param[in]  pixels Buffer to sample, width * height RGB888 pixels.
 * @param[in]  width  Buffer width in pixels.
 * @param[in]  height Buffer height in pixels.
 * @param[in]  x      Horizontal coordinate, may fall outside the buffer.
 * @param[in]  y      Vertical coordinate, may fall outside the buffer.
 * @param[out] red    Receives the red channel as 0..255.
 * @param[out] green  Receives the green channel as 0..255.
 * @param[out] blue   Receives the blue channel as 0..255.
 */
static void sample_pixel(const uint8_t *pixels, int width, int height, float x, float y, float *red,
	float *green, float *blue)
{
	int ix = (int)x;
	int iy = (int)y;
	const uint8_t *pixel;

	/* clamp the lookup to the artwork instead of wrapping or skipping */
	if (ix < 0) {
		ix = 0;
	} else if (ix >= width) {
		ix = width - 1;
	}
	if (iy < 0) {
		iy = 0;
	} else if (iy >= height) {
		iy = height - 1;
	}
	pixel = &pixels[((size_t)iy * width + ix) * GLASS_BPP];
	*blue = pixel[0];
	*green = pixel[1];
	*red = pixel[2];
}

/**
 * @brief Linearly interpolate between two channel values.
 *
 * @param[in] first  Value at @p amount 0.
 * @param[in] second Value at @p amount 1.
 * @param[in] amount Blend weight, 0.0 to 1.0.
 * @return The interpolated value.
 */
static float mix_channel(float first, float second, float amount)
{
	return first + (second - first) * amount;
}

/**
 * @brief Scale a rim opacity from the 86-based reference to the 255-based one.
 *
 * @param[in] opacity Reference opacity, 0..255.
 * @return The scaled opacity, saturated at 255.
 */
static uint8_t scale_opacity(uint8_t opacity)
{
	uint32_t scaled = (uint32_t)opacity * GLASS_EDGE_STRENGTH / 86U;

	return (uint8_t)(scaled > 255U ? 255U : scaled);
}

/* Signed distance to a rounded rectangle, negative inside. */
/**
 * @brief Signed distance from a point to a rounded rectangle, negative inside.
 *
 * @details Uses the standard rounded-box distance: the distance to the inner
 *          rectangle plus the corner radius, combined with the outside-only
 *          length of the component-wise offset. The result drives the three
 *          one-pixel rim bands.
 *
 * @param[in] x           Point X relative to the rectangle centre.
 * @param[in] y           Point Y relative to the rectangle centre.
 * @param[in] half_width  Half of the rectangle width.
 * @param[in] half_height Half of the rectangle height.
 * @param[in] radius      Corner radius, clamped by the caller.
 * @return Signed distance in pixels; negative inside the rounded rectangle.
 */
static float rounded_distance(float x, float y, float half_width, float half_height, float radius)
{
	float qx = fabsf(x) - (half_width - radius);
	float qy = fabsf(y) - (half_height - radius);
	float outside_x = qx > 0.0f ? qx : 0.0f;
	float outside_y = qy > 0.0f ? qy : 0.0f;

	return sqrtf(outside_x * outside_x + outside_y * outside_y) - radius +
		fminf(fmaxf(qx, qy), 0.0f);
}

/**
 * @brief Render a glass surface into its pixel buffer.
 *
 * @details Samples the blurred backdrop at the widget's screen coordinates and
 *          tints it with UI_GLASS_TINT at the surface opacity. It then uses the
 *          rounded-rectangle signed distance to paint three concentric one-pixel
 *          rims (white, the wallpaper colour above, the wallpaper colour below),
 *          adds the bottom refraction segment, lifts the whole surface slightly
 *          towards white, and invalidates the widget so LVGL repaints it. The
 *          wallpaper colours above and below are sampled four pixels outside the
 *          widget, which is where the refracted edge should read from.
 *
 * @param[in,out] surface Surface to render; @c pixels must already be allocated
 *                        at width * height * GLASS_BPP bytes.
 */
static void render_surface(struct glass_surface *surface)
{
	const lv_image_dsc_t *sharp = backdrop_image();
	const uint8_t *sharp_pixels = sharp->data;
	const uint8_t *soft_pixels = backdrop_blurred();
	int source_width = (int)sharp->header.w;
	int source_height = (int)sharp->header.h;
	float opacity = surface->opacity / 255.0f;
	float tint_red = (UI_GLASS_TINT >> 16) & 0xffU;
	float tint_green = (UI_GLASS_TINT >> 8) & 0xffU;
	float tint_blue = UI_GLASS_TINT & 0xffU;
	float top_red;
	float top_green;
	float top_blue;
	float bottom_red;
	float bottom_green;
	float bottom_blue;
	lv_area_t coords;
	int width = surface->width;
	int height = surface->height;
	float half_width = width * 0.5f;
	float half_height = height * 0.5f;
	float radius = fminf((float)surface->radius, fminf(half_width, half_height));

	lv_obj_get_coords(surface->object, &coords);
	/* wallpaper colours just above and below the sheet feed the rim and refraction */
	sample_pixel(sharp_pixels, source_width, source_height, coords.x1 + half_width,
		coords.y1 - 4.0f, &top_red, &top_green, &top_blue);
	sample_pixel(sharp_pixels, source_width, source_height, coords.x1 + half_width,
		coords.y2 + 4.0f, &bottom_red, &bottom_green, &bottom_blue);

	for (int y = 0; y < height; y++) {
		float py = y + 0.5f - half_height;

		for (int x = 0; x < width; x++) {
			float px = x + 0.5f - half_width;
			float red;
			float green;
			float blue;
			float distance;
			uint8_t *out = &surface->pixels[((size_t)y * width + x) * GLASS_BPP];

			/* start from the blurred artwork and mix in the glass tint */
			sample_pixel(soft_pixels, source_width, source_height, coords.x1 + x + 0.5f,
				coords.y1 + y + 0.5f, &red, &green, &blue);
			red = mix_channel(red, tint_red, opacity);
			green = mix_channel(green, tint_green, opacity);
			blue = mix_channel(blue, tint_blue, opacity);

			distance = rounded_distance(px, py, half_width, half_height, radius);
			/* three concentric rims: white, the colour above, the colour below */
			for (int ring = 0; ring < 3; ring++) {
				if (distance <= -(float)ring && distance > -(float)(ring + 1)) {
					float ring_red;
					float ring_green;
					float ring_blue;
					float amount;
					uint8_t ring_opacity;

					/* outermost ring is a white specular line */
					if (ring == 0) {
						amount = 92.0f / 255.0f;
						ring_red = mix_channel(tint_red, 255.0f, amount);
						ring_green = mix_channel(tint_green, 255.0f, amount);
						ring_blue = mix_channel(tint_blue, 255.0f, amount);
						ring_opacity = scale_opacity(72);
					/* middle ring picks up the wallpaper above the sheet */
					} else if (ring == 1) {
						amount = 104.0f / 255.0f;
						ring_red = mix_channel(tint_red, top_red, amount);
						ring_green = mix_channel(tint_green, top_green, amount);
						ring_blue = mix_channel(tint_blue, top_blue, amount);
						ring_opacity = scale_opacity(26);
					/* inner ring picks up the wallpaper below the sheet */
					} else {
						amount = 132.0f / 255.0f;
						ring_red = mix_channel(tint_red, bottom_red, amount);
						ring_green = mix_channel(tint_green, bottom_green, amount);
						ring_blue = mix_channel(tint_blue, bottom_blue, amount);
						ring_opacity = scale_opacity(7);
					}
					amount = ring_opacity / 255.0f;
					red = mix_channel(red, ring_red, amount);
					green = mix_channel(green, ring_green, amount);
					blue = mix_channel(blue, ring_blue, amount);
				}
			}
			/* the bottom refraction of the material */
			/* a short band near the bottom-right corner reads the colour below the sheet */
			if (y == height - 2 && x <= (int)(width - radius) &&
				x >= (int)(width - radius - (width - 2.0f * radius) * 0.36f)) {
				float amount = scale_opacity(34) / 255.0f;

				red = mix_channel(red, mix_channel(bottom_red, tint_red, 72.0f / 255.0f), amount);
				green = mix_channel(green, mix_channel(bottom_green, tint_green, 72.0f / 255.0f), amount);
				blue = mix_channel(blue, mix_channel(bottom_blue, tint_blue, 72.0f / 255.0f), amount);
			}
			/* glass tint plus a small white lift keeps the material luminous */
			red += 16.0f;
			green += 18.0f;
			blue += 20.0f;
			/* clamp and store as B, G, R */
			out[0] = (uint8_t)(blue < 0.0f ? 0 : blue > 255.0f ? 255 : (int)blue);
			out[1] = (uint8_t)(green < 0.0f ? 0 : green > 255.0f ? 255 : (int)green);
			out[2] = (uint8_t)(red < 0.0f ? 0 : red > 255.0f ? 255 : (int)red);
		}
	}
	lv_obj_invalidate(surface->object);
}

/**
 * @brief LV_EVENT_DELETE handler that returns a glass surface slot to the pool.
 *
 * @details Detaches the surface from the widget, frees the rendered pixel
 *          buffer, and clears @c used so the slot can be reused. Called with
 *          the slot as user data.
 *
 * @param[in] event LVGL delete event carrying the surface pointer.
 */
static void glass_deleted(lv_event_t *event)
{
	struct glass_surface *surface = lv_event_get_user_data(event);

	if (surface == NULL || !surface->used) {
		return;
	}
	surface->object = NULL;
	vPortFree(surface->pixels);
	surface->pixels = NULL;
	surface->used = false;
}

/**
 * @brief Attach a glass surface with the default tint strength.
 *
 * @param[in] object Widget to decorate.
 * @param[in] radius Corner radius in pixels.
 */
void glass_attach(lv_obj_t *object, int32_t radius)
{
	glass_attach_tinted(object, radius, GLASS_DEFAULT_OPACITY);
}

/**
 * @brief Attach a rendered glass surface of an explicit tint strength.
 *
 * @details Forces a layout pass so the widget's final size is known, skips
 *          empty widgets, then claims the first free pool slot, allocates its
 *          pixel buffer from the FreeRTOS heap, fills in the LVGL image
 *          descriptor, installs it as the widget background at full opacity,
 *          registers the delete handler, and renders the surface. If no slot or
 *          no memory is available the widget is left without a glass layer.
 *
 * @param[in] object  Widget to decorate; receives the image as background.
 * @param[in] radius  Corner radius in pixels.
 * @param[in] opacity Tint strength, 0..255.
 */
void glass_attach_tinted(lv_obj_t *object, int32_t radius, uint8_t opacity)
{
	lv_obj_update_layout(object);
	int32_t width = lv_obj_get_width(object);
	int32_t height = lv_obj_get_height(object);

	if (width <= 0 || height <= 0) {
		return;
	}
	for (uint32_t index = 0U; index < ARRAY_SIZE(surfaces); index++) {
		struct glass_surface *surface = &surfaces[index];
		size_t bytes = (size_t)width * (size_t)height * GLASS_BPP;

		if (surface->used) {
			continue;
		}
		memset(surface, 0, sizeof(*surface));
		/* the rendered surface lives on the heap for as long as the widget */
		surface->pixels = pvPortMalloc(bytes);
		if (surface->pixels == NULL) {
			return;
		}
		surface->object = object;
		surface->width = width;
		surface->height = height;
		surface->radius = radius;
		surface->opacity = opacity;
		surface->image.header.magic = LV_IMAGE_HEADER_MAGIC;
		surface->image.header.cf = LV_COLOR_FORMAT_RGB888;
		surface->image.header.w = (uint32_t)width;
		surface->image.header.h = (uint32_t)height;
		surface->image.header.stride = (uint32_t)width * GLASS_BPP;
		surface->image.data_size = (uint32_t)bytes;
		surface->image.data = surface->pixels;
		surface->used = true;
		lv_obj_set_style_bg_image_src(object, &surface->image, 0);
		lv_obj_set_style_bg_image_opa(object, LV_OPA_COVER, 0);
		lv_obj_add_event_cb(object, glass_deleted, LV_EVENT_DELETE, surface);
		render_surface(surface);
		return;
	}
}

/**
 * @brief Create a non-interactive coloured overlay child.
 *
 * @details Sets a fixed size and position, a flat rounded background, no border
 *          or padding, and disables clicks and scrolling so the overlay never
 *          steals input from its parent.
 *
 * @param[in] parent  Parent object.
 * @param[in] x       Left edge in the parent's coordinate space.
 * @param[in] y       Top edge in the parent's coordinate space.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] radius  Corner radius in pixels.
 * @param[in] color   Background colour, 0xRRGGBB.
 * @param[in] opacity Background opacity, 0..255.
 * @return The new overlay object.
 */
static lv_obj_t *make_overlay(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
	int32_t radius, uint32_t color, uint8_t opacity)
{
	lv_obj_t *overlay = lv_obj_create(parent);

	lv_obj_set_size(overlay, width, height);
	lv_obj_set_pos(overlay, x, y);
	lv_obj_set_style_bg_color(overlay, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(overlay, opacity, 0);
	lv_obj_set_style_border_width(overlay, 0, 0);
	lv_obj_set_style_radius(overlay, radius, 0);
	lv_obj_set_style_pad_all(overlay, 0, 0);
	lv_obj_set_clickable(overlay, false);
	lv_obj_set_scrollable(overlay, false);
	return overlay;
}

/**
 * @brief Attach a frosted highlight sheet to a widget.
 *
 * @details Makes the widget transparent with a one-pixel white border at 110
 *          opacity, clips to the corner radius, and adds a soft white shadow.
 *          A blurred-backdrop image child is positioned so the wallpaper lines
 *          up with the widget's screen position, and a 30-opacity white wash is
 *          laid over it to turn the blur into a glass highlight. The rim itself
 *          is the object border.
 *
 * @param[in] object Widget to highlight; its background is replaced.
 * @param[in] radius Corner radius in pixels.
 * @return The backdrop image child, to be passed to glass_highlight_move() when
 *         the widget moves.
 */
lv_obj_t *glass_highlight_attach(lv_obj_t *object, int32_t radius)
{
	lv_area_t coords;
	lv_obj_t *image;
	int32_t width;
	int32_t height;

	lv_obj_update_layout(object);
	width = lv_obj_get_width(object);
	height = lv_obj_get_height(object);
	/* transparent fill, a fine white rim, and a soft drop shadow */
	lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(object, 1, 0);
	lv_obj_set_style_border_color(object, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_border_opa(object, 110, 0);
	lv_obj_set_style_radius(object, radius, 0);
	lv_obj_set_style_clip_corner(object, true, 0);
	lv_obj_set_style_pad_all(object, 0, 0);
	lv_obj_set_style_shadow_width(object, 14, 0);
	lv_obj_set_style_shadow_color(object, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_shadow_opa(object, 40, 0);
	lv_obj_set_style_shadow_ofs_y(object, 2, 0);
	lv_obj_set_clickable(object, false);
	lv_obj_set_scrollable(object, false);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
	lv_obj_update_layout(object);
	lv_obj_get_coords(object, &coords);

	/* align the blurred wallpaper with the widget's absolute position and send it behind */
	image = lv_image_create(object);
	lv_image_set_src(image, backdrop_blurred_image());
	lv_obj_set_pos(image, -coords.x1 - 1, -coords.y1 - 1);
	lv_obj_set_scrollable(image, false);
	lv_obj_move_background(image);

	/* a light wash turns the blur into a glass highlight; the rim itself is the object border */
	make_overlay(object, 1, 1, width - 2, height - 2, radius > 2 ? radius - 1 : radius, 0xffffff, 30);
	return image;
}

/**
 * @brief Realign the highlight backdrop after its owner moved.
 *
 * @details Repositions the blurred image so its top-left stays one pixel
 *          outside the widget's current absolute coordinates. Null arguments
 *          are ignored so callers do not need to guard teardown.
 *
 * @param[in] object   Widget that owns the highlight.
 * @param[in] backdrop Backdrop child returned by glass_highlight_attach().
 */
void glass_highlight_move(lv_obj_t *object, lv_obj_t *backdrop)
{
	lv_area_t coords;

	if (object == NULL || backdrop == NULL) {
		return;
	}
	lv_obj_get_coords(object, &coords);
	lv_obj_set_pos(backdrop, -coords.x1 - 1, -coords.y1 - 1);
}
