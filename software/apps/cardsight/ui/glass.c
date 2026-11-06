// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Glass material for the CardSight UI, following the AI Passport glass system: a translucent
 * tinted sheet over the artwork, three one-pixel edge rings mixed with the wallpaper above and
 * below, a short top specular and a bottom refraction segment. Everything is RGB888.
 */

#include "ui/glass.h"

#include "FreeRTOS.h"
#include <bits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ui/backdrop.h"
#include "ui/theme.h"

#define GLASS_MAX_SURFACES   20U
#define GLASS_EDGE_STRENGTH  126U
#define GLASS_DEFAULT_OPACITY 154U
#define GLASS_BPP	     3U

struct glass_surface {
	lv_obj_t *object;
	lv_image_dsc_t image;
	uint8_t *pixels;
	int32_t width;
	int32_t height;
	int32_t radius;
	uint8_t opacity;
	bool used;
};

static struct glass_surface surfaces[GLASS_MAX_SURFACES];

/* LVGL RGB888 keeps B, G, R in memory. */
static void sample_pixel(const uint8_t *pixels, int width, int height, float x, float y, float *red,
	float *green, float *blue)
{
	int ix = (int)x;
	int iy = (int)y;
	const uint8_t *pixel;

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

static float mix_channel(float first, float second, float amount)
{
	return first + (second - first) * amount;
}

static uint8_t scale_opacity(uint8_t opacity)
{
	uint32_t scaled = (uint32_t)opacity * GLASS_EDGE_STRENGTH / 86U;

	return (uint8_t)(scaled > 255U ? 255U : scaled);
}

/* Signed distance to a rounded rectangle, negative inside. */
static float rounded_distance(float x, float y, float half_width, float half_height, float radius)
{
	float qx = fabsf(x) - (half_width - radius);
	float qy = fabsf(y) - (half_height - radius);
	float outside_x = qx > 0.0f ? qx : 0.0f;
	float outside_y = qy > 0.0f ? qy : 0.0f;

	return sqrtf(outside_x * outside_x + outside_y * outside_y) - radius +
		fminf(fmaxf(qx, qy), 0.0f);
}

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

					if (ring == 0) {
						amount = 92.0f / 255.0f;
						ring_red = mix_channel(tint_red, 255.0f, amount);
						ring_green = mix_channel(tint_green, 255.0f, amount);
						ring_blue = mix_channel(tint_blue, 255.0f, amount);
						ring_opacity = scale_opacity(72);
					} else if (ring == 1) {
						amount = 104.0f / 255.0f;
						ring_red = mix_channel(tint_red, top_red, amount);
						ring_green = mix_channel(tint_green, top_green, amount);
						ring_blue = mix_channel(tint_blue, top_blue, amount);
						ring_opacity = scale_opacity(26);
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
			/* a short specular at the top-left and refraction at the bottom-right */
			if (y == 1 && x >= (int)radius &&
				x <= (int)(radius + (width - 2.0f * radius) * 0.30f)) {
				float amount = scale_opacity(76) / 255.0f;

				red = mix_channel(red, mix_channel(top_red, 255.0f, 128.0f / 255.0f), amount);
				green = mix_channel(green, mix_channel(top_green, 255.0f, 128.0f / 255.0f), amount);
				blue = mix_channel(blue, mix_channel(top_blue, 255.0f, 128.0f / 255.0f), amount);
			}
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
			out[0] = (uint8_t)(blue < 0.0f ? 0 : blue > 255.0f ? 255 : (int)blue);
			out[1] = (uint8_t)(green < 0.0f ? 0 : green > 255.0f ? 255 : (int)green);
			out[2] = (uint8_t)(red < 0.0f ? 0 : red > 255.0f ? 255 : (int)red);
		}
	}
	lv_obj_invalidate(surface->object);
}

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

void glass_attach(lv_obj_t *object, int32_t radius)
{
	glass_attach_tinted(object, radius, GLASS_DEFAULT_OPACITY);
}

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

lv_obj_t *glass_highlight_attach(lv_obj_t *object, int32_t radius)
{
	lv_area_t coords;
	lv_obj_t *image;
	int32_t width;
	int32_t height;

	lv_obj_update_layout(object);
	width = lv_obj_get_width(object);
	height = lv_obj_get_height(object);
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

	image = lv_image_create(object);
	lv_image_set_src(image, backdrop_blurred_image());
	lv_obj_set_pos(image, -coords.x1 - 1, -coords.y1 - 1);
	lv_obj_set_scrollable(image, false);
	lv_obj_move_background(image);

	/* a light wash turns the blur into a glass highlight; the rim itself is the object border */
	make_overlay(object, 1, 1, width - 2, height - 2, radius > 2 ? radius - 1 : radius, 0xffffff, 30);
	return image;
}

void glass_highlight_move(lv_obj_t *object, lv_obj_t *backdrop)
{
	lv_area_t coords;

	if (object == NULL || backdrop == NULL) {
		return;
	}
	lv_obj_get_coords(object, &coords);
	lv_obj_set_pos(backdrop, -coords.x1 - 1, -coords.y1 - 1);
}
