#ifndef OBS_TEXT_PTHREAD_H
#define OBS_TEXT_PTHREAD_H

#include <pthread.h>

struct tp_texture
{
	// data from the thread
	uint32_t width, height;
	gs_texture_t *tex;
	uint8_t *surface;
	uint64_t time_ns;
	bool config_updated;

	struct tp_texture *next;
};

enum {
	ALIGN_LEFT = 0,
	ALIGN_CENTER = 1,
	ALIGN_RIGHT = 2,
	ALIGN_JUSTIFY = 4,
};

enum {
	OUTLINE_ROUND = 0,
	OUTLINE_BEVEL = 1,
	OUTLINE_RECT = 2,
	OUTLINE_SHARP = 4,
};

struct tp_config
{
	char *font_name;
	char *font_style;
	uint32_t font_size;
	uint32_t font_flags;
	// TODO: font weight stretch gravity
	char *text;
	uint32_t color;
	uint32_t width, height;
	bool shrink_size;
	uint32_t align;
	bool auto_dir;
	int wrapmode;
	int32_t indent;
	int ellipsize;
	int spacing;
	bool outline;
	uint32_t outline_color;
	uint32_t outline_width;
	uint32_t outline_blur;
	uint32_t outline_shape;
	bool outline_blur_gaussian;
	bool shadow;
	uint32_t shadow_color;
	int32_t shadow_x, shadow_y;
};

struct tp_source
{
	// config
	// write from main
	// read from thread
	pthread_mutex_t config_mutex;
	struct tp_config config;
	bool config_updated;
	volatile bool running;

	// new texture
	// write from thread
	// read from main and set to NULL
	pthread_mutex_t tex_mutex;
	struct tp_texture *tex_new;
	volatile bool text_updating;

	// internal use for main
	struct tp_texture *textures;

	// threads
	pthread_t thread;
};

void tp_thread_start(struct tp_source *src);
void tp_thread_end(struct tp_source *src);

#define BFREE_IF_NONNULL(x) \
	if (x) {            \
		bfree(x);   \
		(x) = NULL; \
	}

static inline void tp_config_destroy_member(struct tp_config *c)
{
	BFREE_IF_NONNULL(c->font_name);
	BFREE_IF_NONNULL(c->font_style);
	BFREE_IF_NONNULL(c->text);
}

static inline void free_texture(struct tp_texture *t)
{
	if (t->tex) {
		obs_enter_graphics();
		for (struct tp_texture *i = t; i; i = i->next) {
			if (i->tex)
				gs_texture_destroy(i->tex);
			i->tex = NULL;
		}
		obs_leave_graphics();
	}
	if (t->surface)
		bfree(t->surface);
	if (t->next)
		free_texture(t->next);
	bfree(t);
}

static inline struct tp_texture *pushback_texture(struct tp_texture *dest, struct tp_texture *n)
{
	if (!dest)
		return n;
	for (struct tp_texture *t = dest;; t = t->next) {
		if (!t->next) {
			t->next = n;
			return dest;
		}
	}
}

static inline struct tp_texture *popfront_texture(struct tp_texture *t)
{
	struct tp_texture *ret = t->next;
	t->next = NULL;
	free_texture(t);
	return ret;
}

void tp_edit_text(struct tp_source *src, char * text);

#endif // OBS_TEXT_PTHREAD_H
