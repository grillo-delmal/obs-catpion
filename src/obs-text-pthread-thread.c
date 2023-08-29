/* obs-text-ptherad-thread.c
 *
 * Copied from https://github.com/norihiro/obs-text-pthread
 * Modified by Grillo del Mal (2023)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
*/
 
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <string.h>
#include <pango/pangocairo.h>
#include <inttypes.h>
#include "obs-text-pthread.h"

#define GAUSSIAN_RANGE 2

static double u32toFR(uint32_t u)
{
	return (double)((u >> 0) & 0xFF) / 255.;
}
static double u32toFG(uint32_t u)
{
	return (double)((u >> 8) & 0xFF) / 255.;
}
static double u32toFB(uint32_t u)
{
	return (double)((u >> 16) & 0xFF) / 255.;
}
static double u32toFA(uint32_t u)
{
	return (double)((u >> 24) & 0xFF) / 255.;
}

static inline int blur_step(int blur)
{
	// only odd number is allowed
	// roughly 16 steps to draw with pango-cairo, then blur by pixel.
	return (blur / 8) | 1;
}

static void tp_stroke_path(cairo_t *cr, PangoLayout *layout, const struct tp_config *config, int offset_x, int offset_y,
			   uint32_t color, int width, int blur)
{
	bool path_preserved = false;
	bool blur_gaussian = config->outline_blur_gaussian;
	const int bs = blur_step(blur);
	int b_end = blur_gaussian ? -blur * GAUSSIAN_RANGE : -blur;
	if (blur && b_end + width <= 0)
		b_end = -width + 1;
	int b_start = blur_gaussian ? blur * GAUSSIAN_RANGE : blur;
	if (bs > 1)
		b_start = b_end + (b_start - b_end + bs - 1) / bs * bs;
	double a_prev = 0.0;
	for (int b = b_start; b >= b_end; b -= bs) {
		double a;
		if (!blur)
			a = 1.0;
		else if (blur_gaussian) {
			int bs1 = bs ? bs + 1 : 0;
			a = 0.5 - erff((float)(b - bs1 * 0.5f) / blur) * 0.5;
		}
		else
			a = 0.5 - b * 0.5 / blur;
		a *= u32toFA(color);

		// skip this loop if quantized alpha code is same as that in the previous.
		if (blur && (int)(a * 255 + 0.5) == (int)(a_prev * 255 + 0.5))
			continue;
		a_prev = a;

		int w = (width + b) * 2;
		if (w < 0)
			break;

		cairo_move_to(cr, offset_x, offset_y);
		cairo_set_source_rgba(cr, u32toFR(color), u32toFG(color), u32toFB(color), a);
		if (w > 0) {
			cairo_set_line_width(cr, w);
			if (config->outline_shape & OUTLINE_BEVEL) {
				cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
			}
			else if (config->outline_shape & OUTLINE_RECT) {
				cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
				cairo_set_miter_limit(cr, 1.999);
			}
			else if (config->outline_shape & OUTLINE_SHARP) {
				cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
				cairo_set_miter_limit(cr, 3.999);
			}
			else {
				cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
			}
			if (!path_preserved)
				pango_cairo_layout_path(cr, layout);
			cairo_stroke_preserve(cr);
			path_preserved = true;
		}
		else {
			pango_cairo_show_layout(cr, layout);
		}
	}

	cairo_surface_flush(cairo_get_target(cr));

	if (bs > 1) {
		cairo_surface_t *surface = cairo_get_target(cr);
		const int w = cairo_image_surface_get_width(surface);
		const int h = cairo_image_surface_get_height(surface);
		uint8_t *data = cairo_image_surface_get_data(surface);
		const int bs1 = bs + 1;
		uint32_t *tmp = bzalloc(sizeof(uint32_t) * w * bs1);
		uint32_t **tt = bzalloc(sizeof(uint32_t *) * bs1);

		for (int k = 0; k < bs1; k++) {
			tt[k] = tmp + w * k;
		}

		const int bs2 = bs / 2;
		const int div = bs * bs;
		for (int k = 0, kt = 0; k < h; k++) {
			const int k2 = k + bs2 < h ? k + bs2 : h - 1;
			const int k1 = k - bs2 - 1;

			for (; kt <= k2; kt++) {
				for (int i = 0; i < w; i++) {
					uint32_t s = data[(i + kt * w) * 4 + 3];
					if (i > 0)
						s += tt[kt % bs1][i - 1];
					if (kt > 0)
						s += +tt[(kt - 1) % bs1][i];
					if (kt > 0 && i > 0)
						s -= tt[(kt - 1) % bs1][i - 1];
					tt[kt % bs1][i] = s;
				}
			}

			for (int i = 0; i < w; i++) {
				const int i2 = i + bs2 < w ? i + bs2 : w - 1;
				const int i1 = i - bs2 - 1;
				uint32_t s = tt[k2 % bs1][i2];
				if (k1 >= 0)
					s -= tt[k1 % bs1][i2];
				if (i1 >= 0)
					s -= tt[k2 % bs1][i1];
				if (k1 >= 0 && i1 >= 0)
					s += tt[k1 % bs1][i1];
				s /= div;
				if (s > 255)
					s = 255;
				data[(i + k * w) * 4 + 3] = s;
			}
		}

		bfree(tmp);
		bfree(tt);
	}
}

static inline uint32_t blend_text_ch(uint32_t xat, uint32_t xb, uint32_t at, uint32_t ab, uint32_t u)
{
	// u: factor for the bottom color
	return xat + xb * ab * u * (255 - at) / (255 * 255 * 255);
}

static inline uint32_t blend_text(uint32_t cat, uint32_t cb, uint32_t u)
{
	uint32_t a_255 = (cat >> 24) * 255 + u * (cb >> 24) - (cat >> 24) * u * (cb >> 24) / 255;
	if (a_255 < 255)
		return 0; // completely transparent
	return ((a_255 / 255) << 24) |
	       (blend_text_ch((cat >> 16) & 0xFF, (cb >> 16) & 0xFF, cat >> 24, cb >> 24, u) << 16) |
	       (blend_text_ch((cat >> 8) & 0xFF, (cb >> 8) & 0xFF, cat >> 24, cb >> 24, u) << 8) |
	       (blend_text_ch((cat)&0xFF, (cb)&0xFF, cat >> 24, cb >> 24, u));
}

static inline void blend_shadow(uint8_t *s, const int stride, const uint32_t h, const uint8_t *ss, uint32_t cs)
{
	uint32_t size = h * stride;
	for (uint32_t i = 0, k = 0; i < size; i += 4, k += 1)
		if (ss[k]) {
			uint32_t ct = s ? s[i] << 16 | s[i + 1] << 8 | s[i + 2] | s[i + 3] << 24 : 0;
			uint32_t c = blend_text(ct, cs, ss[k]);
			s[i] = c >> 16;
			s[i + 1] = c >> 8;
			s[i + 2] = c;
			s[i + 3] = c >> 24;
		}
}

static struct tp_texture *tp_draw_texture(struct tp_config *config, char *text)
{
	struct tp_texture *n = bzalloc(sizeof(struct tp_texture));

	int outline_width = config->outline ? config->outline_width : 0;
	int outline_blur = config->outline ? config->outline_blur : 0;
	bool outline_blur_gaussian = config->outline_blur_gaussian;
	int outline_width_blur = outline_width + (outline_blur_gaussian ? outline_blur * GAUSSIAN_RANGE : outline_blur);
	if (config->outline_shape & OUTLINE_SHARP)
		outline_width_blur *= 2;
	int shadow_abs_x = config->shadow ? abs(config->shadow_x) : 0;
	int shadow_abs_y = config->shadow ? abs(config->shadow_y) : 0;
	int offset_x = outline_width_blur + (config->shadow && config->shadow_x < 0 ? -config->shadow_x : 0);
	int offset_y = outline_width_blur + (config->shadow && config->shadow_y < 0 ? -config->shadow_y : 0);

	uint32_t body_width = config->width;
	uint32_t surface_width = body_width + outline_width_blur * 2 + shadow_abs_x;
	uint32_t surface_height = config->height + outline_width_blur * 2 + shadow_abs_y;

	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, surface_width);
	n->surface = bzalloc(stride * surface_height);

	cairo_surface_t *surface = cairo_image_surface_create_for_data(n->surface, CAIRO_FORMAT_ARGB32, surface_width,
								       surface_height, stride);

	cairo_t *cr = cairo_create(surface);

	PangoLayout *layout = pango_cairo_create_layout(cr);

	blog(LOG_DEBUG, "[catpion] font name=<%s> style=<%s> size=%d flags=0x%X\n", config->font_name, config->font_style,
	      config->font_size, config->font_flags);
	PangoFontDescription *desc = pango_font_description_new();
	pango_font_description_set_family(desc, config->font_name);
	pango_font_description_set_weight(desc, (config->font_flags & OBS_FONT_BOLD) ? PANGO_WEIGHT_BOLD : 0);
	pango_font_description_set_style(desc, (config->font_flags & OBS_FONT_ITALIC) ? PANGO_STYLE_ITALIC : 0);
	pango_font_description_set_size(desc, (config->font_size * PANGO_SCALE * 2) /
						      3); // Scaling to approximate GDI text pts
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	if (config->align & ALIGN_CENTER)
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	else if (config->align & ALIGN_RIGHT)
		pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
	else // ALIGN_LEFT
		pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	pango_layout_set_justify(layout, !!(config->align & ALIGN_JUSTIFY));
	pango_layout_set_indent(layout, config->indent * PANGO_SCALE);

	pango_layout_set_width(layout, body_width << 10);
	pango_layout_set_auto_dir(layout, config->auto_dir);
	pango_layout_set_wrap(layout, config->wrapmode);
	pango_layout_set_ellipsize(layout, config->ellipsize);
	pango_layout_set_spacing(layout, config->spacing * PANGO_SCALE);

	pango_layout_set_text(layout, text, -1);

	PangoRectangle ink_rect, logical_rect;
	pango_layout_get_extents(layout, &ink_rect, &logical_rect);
	uint32_t surface_ink_height = PANGO_PIXELS_FLOOR(ink_rect.height) + PANGO_PIXELS_FLOOR(ink_rect.y) +
				      outline_width_blur * 2 + shadow_abs_y;
	uint32_t surface_ink_height1 = surface_height > surface_ink_height ? surface_ink_height : surface_height;

	if (outline_width_blur > 0) {
		blog(LOG_DEBUG, "[catpion] stroking outline width=%d\n", outline_width);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		tp_stroke_path(cr, layout, config, offset_x, offset_y, config->outline_color, outline_width,
			       outline_blur);

		// overwrite outline color
		uint32_t size = stride * surface_height;
		uint8_t *ptr = n->surface;
		uint8_t c[4] = {config->outline_color >> 16, config->outline_color >> 8, config->outline_color, 0};
		for (uint32_t i = 0; i < size; i += 4) {
			int a = ptr[3];
			for (int k = 0; k < 3; k++) {
				int x = a * c[k] / 255;
				if (x > 255)
					x = 255;
				ptr[k] = x;
			}
			ptr += 4;
		}
	}

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	tp_stroke_path(cr, layout, config, offset_x, offset_y, config->color, 0, 0);

	if (shadow_abs_x || shadow_abs_y) {
		uint8_t *surface_shadow = bzalloc(stride * surface_height);
		uint8_t *dst = surface_shadow;
		uint8_t *src = n->surface;
		if (config->shadow_x > 0)
			dst += shadow_abs_x;
		else
			src += shadow_abs_x * 4;
		if (config->shadow_y > 0)
			dst += shadow_abs_y * stride / 4;
		else
			src += shadow_abs_y * stride;

		for (int y = 0; y < (int)surface_ink_height1 - shadow_abs_y; y++) {
			uint8_t *d = dst;
			dst += stride / 4;
			uint8_t *s = src + 3;
			src += stride;
			for (int x = 0; x < (int)surface_width - shadow_abs_x; x++) {
				*d = *s;
				d += 1;
				s += 4;
			}
		}

		blend_shadow(n->surface, stride, surface_ink_height1, surface_shadow, config->shadow_color);
		bfree(surface_shadow);
	}

	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	if (config->shrink_size) {
		int xoff = PANGO_PIXELS_FLOOR(logical_rect.x);
		if (xoff < 0) {
			n->width = PANGO_PIXELS_CEIL(logical_rect.x + logical_rect.width) + outline_width_blur * 2 +
				   shadow_abs_x;
			xoff = 0;
		}
		else
			n->width = PANGO_PIXELS_CEIL(logical_rect.width) + outline_width_blur * 2 + shadow_abs_x;
		if (n->width > surface_width)
			n->width = surface_width;
		n->height =
			PANGO_PIXELS_CEIL(logical_rect.y + logical_rect.height) + outline_width_blur * 2 + shadow_abs_y;
		if (n->height > surface_height)
			n->height = surface_height;
		if (n->width != surface_width) {
			uint32_t new_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, n->width);
			for (uint32_t y = 1; y < n->height; y++) {
				memmove(n->surface + new_stride * y, n->surface + stride * y + xoff * 4, new_stride);
			}
		}
	}
	else {
		n->width = surface_width;
		n->height = surface_height;
	}

	blog(LOG_DEBUG, "[catpion] tp_draw_texture end: width=%d height=%d \n", n->width, n->height);

	return n;
}

bool tp_compare_stat(const struct stat *a, const struct stat *b)
{
	if (a->st_ino != b->st_ino)
		return true;
	if (a->st_size != b->st_size)
		return true;
#ifdef __USE_XOPEN2K8
	if (memcmp(&a->st_mtim, &b->st_mtim, sizeof(struct timespec)))
		return true;
#else // __USE_XOPEN2K8
	if (a->st_mtime != b->st_mtime)
		return true;
#ifdef _STATBUF_ST_NSEC
	if (a->st_mtimensec != b->st_mtimensec)
		return true;
#endif // _STATBUF_ST_NSEC
#endif // __USE_XOPEN2K8
	return false;
}

static inline bool is_printable(const char *t)
{
	for (; *t; t++) {
		const char c = *t;
		if (!(c == ' ' || c == '\n' || c == '\t' || c == '\r'))
			return true;
	}
	return false;
}

void tp_edit_text(struct tp_source *src, char * text)
{
	pthread_mutex_lock(&src->config_mutex);
	BFREE_IF_NONNULL(src->config.text);
	src->config.text = bstrdup(text);
	src->config_updated = 1;
	pthread_mutex_unlock(&src->config_mutex);
}


static void *tp_thread_main(void *data)
{
	struct tp_source *src = data;

	struct stat st_prev = {0};
	struct tp_config config_prev = {0};
	bool b_printable_prev = false;

	setpriority(PRIO_PROCESS, 0, 19);
	os_set_thread_name("text-pthread");

	while (src->running) {
		os_sleep_ms(33);

		pthread_mutex_lock(&src->config_mutex);

		bool config_updated = src->config_updated;
		bool text_updated = false;

		// check config and copy
		if (config_updated) {
			if (config_prev.text && src->config.text &&
			    strcmp(config_prev.text, src->config.text))
				text_updated = true;

			BFREE_IF_NONNULL(config_prev.font_name);
			BFREE_IF_NONNULL(config_prev.font_style);
			BFREE_IF_NONNULL(config_prev.text);
			memcpy(&config_prev, &src->config, sizeof(struct tp_config));
			config_prev.font_name = bstrdup(src->config.font_name);
			config_prev.font_style = bstrdup(src->config.font_style);
			config_prev.text = bstrdup(src->config.text);
			src->config_updated = 0;
		}

		pthread_mutex_unlock(&src->config_mutex);

		// TODO: how long will it take to draw a new texture?
		// If it takes much longer than frame rate, it should notify the main thread to start fade-out.

		// load file if changed and draw
		if (config_updated || text_updated) {
			uint64_t time_ns = os_gettime_ns();
			char *text = config_prev.text;
			bool b_printable = text ? is_printable(text) : 0;

			// make an early notification
			if (b_printable) {
				os_atomic_set_bool(&src->text_updating, true);
			}

			struct tp_texture *tex;
			if (b_printable) {
				tex = tp_draw_texture(&config_prev, text);
			}
			else {
				tex = bzalloc(sizeof(struct tp_texture));
			}
			tex->time_ns = time_ns;
			tex->config_updated = config_updated && !text_updated;

			pthread_mutex_lock(&src->tex_mutex);
			src->tex_new = pushback_texture(src->tex_new, tex);
			tex = NULL;
			pthread_mutex_unlock(&src->tex_mutex);

			blog(LOG_DEBUG, "[catpion] tp_draw_texture & tp_draw_texture takes %f ms\n", (os_gettime_ns() - time_ns) * 1e-6);

			if (text_updated)
				b_printable_prev = b_printable;

		}
	}

	tp_config_destroy_member(&config_prev);
	return NULL;
}

void tp_thread_start(struct tp_source *src)
{
	src->running = true;
	pthread_create(&src->thread, NULL, tp_thread_main, src);
}

void tp_thread_end(struct tp_source *src)
{
	src->running = false;
	pthread_join(src->thread, NULL);
}
