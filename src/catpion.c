/* catpion.c
 *
 * Copyright (C) 2023 by Grillo del Mal
 *
 * Contains GPL2+ Licensed code fragments from:
 * * Copyright 2022 Dimitris Papaioannou <dimtpap@protonmail.com>
 * * Copyright 2022 abb128
 * * https://github.com/norihiro/obs-text-pthread
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

#include "catpion.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <pipewire/pipewire.h>
#include <pango/pangocairo.h>
#include <april_api.h>

#include "pipewire-audio.h"
#include "obs-text-pthread.h"
#include "line-gen.h"
#include "model.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("catpion", "en-US")

static gs_effect_t *textalpha_effect = NULL;

#define tp_data_get_color(s, c) tp_data_get_color2(s, c, c ".alpha")
static inline uint32_t tp_data_get_color2(obs_data_t *settings, const char *color, const char *alpha)
{
	return ((uint32_t)obs_data_get_int(settings, color) & 0xFFFFFF) |
	       ((uint32_t)obs_data_get_int(settings, alpha) & 0xFF) << 24;
}

#define tp_data_add_color(props, c, t)                                                                 \
	{                                                                                              \
		obs_properties_add_color(props, c, t);                                                 \
		obs_properties_add_int_slider(props, c ".alpha", obs_module_text("Alpha"), 0, 255, 1); \
	}

#define tp_set_visible(props, name, en)                                 \
	{                                                               \
		obs_property_t *prop = obs_properties_get(props, name); \
		if (prop)                                               \
			obs_property_set_visible(prop, en);             \
	}

static inline uint64_t max_u64(uint64_t a, uint64_t b)
{
	if (a > b)
		return a;
	return b;
}

struct target_node {
	const char *friendly_name;
	const char *name;
	uint32_t serial;
	uint32_t channels;

	struct spa_hook node_listener;

	struct obs_audio_caption_src *acs;
};

void handler(void *data, AprilResultType result, size_t count, const AprilToken *tokens) {
	struct obs_audio_caption_src *acs = data;

    switch(result) {
        case APRIL_RESULT_RECOGNITION_PARTIAL:
        case APRIL_RESULT_RECOGNITION_FINAL:
        {
            line_generator_update(&acs->lg, count, tokens);
            if(result == APRIL_RESULT_RECOGNITION_FINAL) {
                line_generator_finalize(&acs->lg);
            }
            line_generator_set_text(&acs->lg);
            break;
        }

        case APRIL_RESULT_ERROR_CANT_KEEP_UP: {
            blog(LOG_WARNING, "[catpion] @__@ can't keep up");
            break;
        }

        case APRIL_RESULT_SILENCE: {
            line_generator_break(&acs->lg);
            line_generator_set_text(&acs->lg);
            break;
        }
    }
}

struct target_node *get_node_by_name(struct obs_audio_caption_src *acs, const char *name)
{
	struct target_node *n;
	obs_pw_audio_proxy_list_for_each(&acs->targets, n)
	{
		if (strcmp(n->name, name) == 0) {
			return n;
		}
	}
	return NULL;
}

struct target_node *get_node_by_serial(struct obs_audio_caption_src *acs, uint32_t serial)
{
	struct target_node *n;
	obs_pw_audio_proxy_list_for_each(&acs->targets, n)
	{
		if (n->serial == serial) {
			return n;
		}
	}
	return NULL;
}

static void start_streaming(struct obs_audio_caption_src *acs, struct target_node *node)
{
	if (!node || !node->channels) {
		return;
	}
	if (acs->session == NULL) return;

	dstr_copy(&acs->target_name, node->name);

	if (pw_stream_get_state(acs->pw.audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
		if (node->serial == acs->connected_serial) {
			/* Already connected to this node */
			return;
		}
		pw_stream_disconnect(acs->pw.audio.stream);
	}

	if (obs_pw_audio_stream_connect(&acs->pw.audio, node->serial, node->channels, acs->model_sample_rate) == 0) {
		acs->connected_serial = node->serial;
		blog(LOG_INFO, "[catpion] %p streaming from %u", acs->pw.audio.stream, node->serial);
	} else {
		acs->connected_serial = SPA_ID_INVALID;
		blog(LOG_WARNING, "[catpion] Error connecting stream %p", acs->pw.audio.stream);
	}

	pw_stream_set_active(acs->pw.audio.stream, obs_source_active(acs->source));
}

static void default_node_cb(void *data, const char *name)
{
	struct obs_audio_caption_src *acs = data;

	blog(LOG_DEBUG, "[catpion] New default device %s", name);

	dstr_copy(&acs->default_info.name, name);

	struct target_node *n = get_node_by_name(acs, name);
	if (n) {
		acs->default_info.node_serial = n->serial;
		if (acs->default_info.autoconnect) {
			start_streaming(acs, n);
		}
	}
}

static void on_node_info_cb(void *data, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	const char *channels = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS);
	if (!channels) {
		return;
	}

	uint32_t c = strtoul(channels, NULL, 10);

	struct target_node *n = data;
	if (n->channels == c) {
		return;
	}
	n->channels = c;

	struct obs_audio_caption_src *acs = n->acs;

	/** If this is the default device and the stream is not already connected to it
	  * or the stream is unconnected and this node has the desired target name */
	if ((acs->default_info.autoconnect && acs->connected_serial != n->serial &&
		 !dstr_is_empty(&acs->default_info.name) && dstr_cmp(&acs->default_info.name, n->name) == 0) ||
		(pw_stream_get_state(acs->pw.audio.stream, NULL) == PW_STREAM_STATE_UNCONNECTED &&
		 !dstr_is_empty(&acs->target_name) && dstr_cmp(&acs->target_name, n->name) == 0)) {
		start_streaming(acs, n);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info_cb,
};

static void register_target_node(struct obs_audio_caption_src *acs, const char *friendly_name, const char *name,
								 uint32_t object_serial, uint32_t global_id)
{
	struct pw_proxy *node_proxy = pw_registry_bind(acs->pw.registry, global_id, PW_TYPE_INTERFACE_Node,
												   PW_VERSION_NODE, sizeof(struct target_node));
	if (!node_proxy) {
		return;
	}

	struct target_node *n = pw_proxy_get_user_data(node_proxy);
	n->friendly_name = bstrdup(friendly_name);
	n->name = bstrdup(name);
	n->serial = object_serial;
	n->channels = 0;
	n->acs = acs;

	obs_pw_audio_proxy_list_append(&acs->targets, node_proxy);

	spa_zero(n->node_listener);
	pw_proxy_add_object_listener(node_proxy, &n->node_listener, &node_events, n);
}

/* Registry */
static void on_global_cb(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
						 const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);

	struct obs_audio_caption_src *acs = data;

	if (!props || !type) {
		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *node_name, *media_class;
		if (!(node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) ||
			!(media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
			return;
		}

		/* Target device */
		if(strcmp(media_class, "Audio/Source") == 0 || strcmp(media_class, "Audio/Source/Virtual") == 0) {

			const char *ser = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
			if (!ser) {
				blog(LOG_WARNING, "[catpion] No object serial found on node %u", id);
				return;
			}
			uint32_t object_serial = strtoul(ser, NULL, 10);

			const char *node_friendly_name = spa_dict_lookup(props, PW_KEY_NODE_NICK);
			if (!node_friendly_name) {
				node_friendly_name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
				if (!node_friendly_name) {
					node_friendly_name = node_name;
				}
			}

			register_target_node(acs, node_friendly_name, node_name, object_serial, id);
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0) {
			return;
		}

		if (!obs_pw_audio_default_node_metadata_listen(
				&acs->default_info.metadata, &acs->pw, id,
				false, default_node_cb, acs)) {
			blog(LOG_WARNING, "[catpion] Failed to get default metadata, cannot detect default audio devices");
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global_cb,
};

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Captions source";
}

static const char *catpion_audio_input_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("Catpion Audio Input");
}

static const char *catpion_audio_output_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("Catpion Audio Output");
}

static void node_destroy_cb(void *data)
{
	struct target_node *n = data;

	struct obs_audio_caption_src *acs = n->acs;
	if (n->serial == acs->connected_serial) {
		if (pw_stream_get_state(acs->pw.audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
			pw_stream_disconnect(acs->pw.audio.stream);
		}
		acs->connected_serial = SPA_ID_INVALID;
	}

	spa_hook_remove(&n->node_listener);

	bfree((void *)n->friendly_name);
	bfree((void *)n->name);
}

static void tp_update(void *data, obs_data_t *settings)
{
	struct tp_source *src = data;

	pthread_mutex_lock(&src->config_mutex);

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		BFREE_IF_NONNULL(src->config.font_name);
		src->config.font_name = bstrdup(obs_data_get_string(font_obj, "face"));

		BFREE_IF_NONNULL(src->config.font_style);
		src->config.font_style = bstrdup(obs_data_get_string(font_obj, "style"));

		src->config.font_size = (uint32_t)obs_data_get_int(font_obj, "size");
		src->config.font_flags = (uint32_t)obs_data_get_int(font_obj, "flags");

		obs_data_release(font_obj);
	}

	BFREE_IF_NONNULL(src->config.text);
	src->config.text = bstrdup("[CC]");

	src->config.color = tp_data_get_color(settings, "color");

	src->config.width = (uint32_t)obs_data_get_int(settings, "width");
	src->config.height = (uint32_t)obs_data_get_int(settings, "height");
	src->config.shrink_size = obs_data_get_bool(settings, "shrink_size");
	src->config.align = obs_data_get_int(settings, "align");
	src->config.auto_dir = obs_data_get_bool(settings, "auto_dir");
	src->config.wrapmode = obs_data_get_int(settings, "wrapmode");
	src->config.indent = obs_data_get_int(settings, "indent");
	src->config.ellipsize = obs_data_get_int(settings, "ellipsize");
	src->config.spacing = obs_data_get_int(settings, "spacing");

	src->config.outline = obs_data_get_bool(settings, "outline");
	src->config.outline_color = tp_data_get_color(settings, "outline_color");
	src->config.outline_width = obs_data_get_int(settings, "outline_width");
	src->config.outline_blur = obs_data_get_int(settings, "outline_blur");
	src->config.outline_blur_gaussian = obs_data_get_bool(settings, "outline_blur_gaussian");
	src->config.outline_shape = obs_data_get_int(settings, "outline_shape");

	src->config.shadow = obs_data_get_bool(settings, "shadow");
	src->config.shadow_color = tp_data_get_color(settings, "shadow_color");
	src->config.shadow_x = obs_data_get_int(settings, "shadow_x");
	src->config.shadow_y = obs_data_get_int(settings, "shadow_y");

	src->config_updated = true;

	pthread_mutex_unlock(&src->config_mutex);
}

void check_cur_session(struct obs_audio_caption_src *acs) {
    AprilConfig config = { 0 };
    config.handler = handler;
    config.userdata = (void*)acs;
    config.flags = APRIL_CONFIG_FLAG_ASYNC_RT_BIT;

	size_t model_id = ModelCurID();

	if(acs->session != NULL){
		if(model_id == acs->model_id){
			return;
		}

		blog(
			LOG_INFO, "[catpion] Captioning session released m[%d] %ld %d", 
			acs->model_id,
			acs->session,
			acs->model_sample_rate);

		pw_thread_loop_lock(acs->pw.thread_loop);
		aas_flush(acs->session);
		aas_free(acs->session);
		acs->session = NULL;
		ModelRelease(acs->model_id);
	}
	else
	{
		pw_thread_loop_lock(acs->pw.thread_loop);
	}

	AprilASRModel model = ModelGet(model_id);
	if(model){
		acs->model_id = model_id;
		ModelTake(model_id);
		line_generator_init(&acs->lg);
		acs->session = aas_create_session(model, config);
		acs->model_sample_rate = aam_get_sample_rate(model);
		blog(
			LOG_INFO, "[catpion] Captioning session created m[%d] %ld %d", 
			model_id,
			acs->session,
			acs->model_sample_rate);
	}

	if(acs->session != NULL){
		line_generator_set_label(&acs->lg, &acs->text_src);
	}
	pw_thread_loop_unlock(acs->pw.thread_loop);
}

void release_session(struct obs_audio_caption_src *acs){
	if(acs->session != NULL){
		aas_flush(acs->session);
		aas_free(acs->session);
		ModelRelease(acs->model_id);
		acs->session = NULL;
	}
}

static void *catpion_audio_input_create(obs_data_t *settings, obs_source_t *source)
{
	struct obs_audio_caption_src *acs = bzalloc(sizeof(struct obs_audio_caption_src));

	if (!obs_pw_audio_instance_init(
			&acs->pw, &registry_events, acs, false, true, acs)) {
		obs_pw_audio_instance_destroy(&acs->pw);

		bfree(acs);
		return NULL;
	}

	acs->source = source;
	acs->default_info.node_serial = SPA_ID_INVALID;
	acs->connected_serial = SPA_ID_INVALID;

	obs_pw_audio_proxy_list_init(&acs->targets, NULL, node_destroy_cb);

	if (obs_data_get_int(settings, "TargetId") != PW_ID_ANY) {
		/** Reset id setting, PipeWire node ids may not persist between sessions.
		  * Connecting to saved target will happen based on the TargetName setting
		  * once target has connected */
		obs_data_set_int(settings, "TargetId", 0);
	} else {
		acs->default_info.autoconnect = true;
	}

	dstr_init_copy(&acs->target_name, obs_data_get_string(settings, "TargetName"));

	obs_pw_audio_instance_sync(&acs->pw);
	pw_thread_loop_wait(acs->pw.thread_loop);
	pw_thread_loop_unlock(acs->pw.thread_loop);

	obs_enter_graphics();
	if (!textalpha_effect) {
		char *f = obs_module_file("textalpha.effect");
		textalpha_effect = gs_effect_create_from_file(f, NULL);
		if (!textalpha_effect)
			blog(LOG_ERROR, "[catpion] Cannot load '%s'", f);
		bfree(f);
	}
	obs_leave_graphics();

	pthread_mutex_init(&acs->text_src.config_mutex, NULL);
	pthread_mutex_init(&acs->text_src.tex_mutex, NULL);

	tp_update(&acs->text_src, settings);

	tp_thread_start(&acs->text_src);

	check_cur_session(acs);
	if(acs->session != NULL){
		acs->lg.to_stream = obs_data_get_bool(settings, "obs_output_caption_stream");
	}
	return acs;
}

static void catpion_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "TargetId", PW_ID_ANY);
	{
		obs_data_t *font_obj = obs_data_create();
		obs_data_set_default_int(font_obj, "size", 64);
		obs_data_set_default_obj(settings, "font", font_obj);
		obs_data_release(font_obj);
	}

	obs_data_set_default_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "color.alpha", 0xFF);

	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_bool(settings, "shrink_size", true);
	obs_data_set_default_bool(settings, "auto_dir", true);
	obs_data_set_default_int(settings, "wrapmode", PANGO_WRAP_WORD);
	obs_data_set_default_int(settings, "ellipsize", PANGO_ELLIPSIZE_NONE);
	obs_data_set_default_int(settings, "spacing", 0);

	obs_data_set_default_int(settings, "outline_color.alpha", 0xFF);

	obs_data_set_default_int(settings, "shadow_x", 2);
	obs_data_set_default_int(settings, "shadow_y", 3);
	obs_data_set_default_int(settings, "shadow_color.alpha", 0xFF);

	obs_data_set_default_bool(settings, "outline_blur_gaussian", true);

	obs_data_set_default_bool(settings, "obs_output_caption_stream", false);
}

static bool tp_prop_outline_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	bool en = settings ? obs_data_get_bool(settings, "outline") : false;
	tp_set_visible(props, "outline_color", en);
	tp_set_visible(props, "outline_color.alpha", en);
	tp_set_visible(props, "outline_width", en);
	tp_set_visible(props, "outline_blur", en);
	tp_set_visible(props, "outline_blur_gaussian", en);
	tp_set_visible(props, "outline_shape", en);

	return true;
}

static bool tp_prop_shadow_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	bool en = settings ? obs_data_get_bool(settings, "shadow") : false;
	tp_set_visible(props, "shadow_color", en);
	tp_set_visible(props, "shadow_color.alpha", en);
	tp_set_visible(props, "shadow_x", en);
	tp_set_visible(props, "shadow_y", en);

	return true;
}

static obs_properties_t *catpion_properties(void *data)
{
	struct obs_audio_caption_src *acs = data;

	obs_properties_t *props;
	obs_property_t *prop;
	props = obs_properties_create();

	prop =
		obs_properties_add_list(props, "TargetId", obs_module_text("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(prop, obs_module_text("Default"), PW_ID_ANY);

	if (!acs->default_info.autoconnect) {
		obs_data_t *settings = obs_source_get_settings(acs->source);
		/* Saved target serial may be different from connected because a previously connected
		   node may have been replaced by one with the same name */
		obs_data_set_int(settings, "TargetId", acs->connected_serial);
		obs_data_release(settings);
	}

	pw_thread_loop_lock(acs->pw.thread_loop);

	struct target_node *n;
	obs_pw_audio_proxy_list_for_each(&acs->targets, n)
	{
		obs_property_list_add_int(prop, n->friendly_name, n->serial);
	}

	pw_thread_loop_unlock(acs->pw.thread_loop);

	obs_properties_add_font(props, "font", obs_module_text("Font"));

	tp_data_add_color(props, "color", obs_module_text("Color"));

	obs_properties_add_int(props, "width", obs_module_text("Width"), 1, 16384, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 1, 16384, 1);
	obs_properties_add_bool(props, "shrink_size", obs_module_text("Automatically shrink size"));

	prop = obs_properties_add_list(props, "align", obs_module_text("Alignment"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Alignment.Left"), ALIGN_LEFT);
	obs_property_list_add_int(prop, obs_module_text("Alignment.Center"), ALIGN_CENTER);
	obs_property_list_add_int(prop, obs_module_text("Alignment.Right"), ALIGN_RIGHT);
	obs_property_list_add_int(prop, obs_module_text("Alignment.Left.Justify"), ALIGN_LEFT | ALIGN_JUSTIFY);
	obs_property_list_add_int(prop, obs_module_text("Alignment.Center.Justify"), ALIGN_CENTER | ALIGN_JUSTIFY);
	obs_property_list_add_int(prop, obs_module_text("Alignment.Right.Justify"), ALIGN_RIGHT | ALIGN_JUSTIFY);

	obs_properties_add_bool(props, "auto_dir", obs_module_text("Calculate the bidirectonal base direction"));

	prop = obs_properties_add_list(props, "wrapmode", obs_module_text("Wrap text"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Wrapmode.Word"), PANGO_WRAP_WORD);
	obs_property_list_add_int(prop, obs_module_text("Wrapmode.Char"), PANGO_WRAP_CHAR);
	obs_property_list_add_int(prop, obs_module_text("Wrapmode.WordChar"), PANGO_WRAP_WORD_CHAR);

	obs_properties_add_int(props, "indent", obs_module_text("Indent"), -32767, 32767, 1);

	prop = obs_properties_add_list(props, "ellipsize", obs_module_text("Ellipsize"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Ellipsize.None"), PANGO_ELLIPSIZE_NONE);
	obs_property_list_add_int(prop, obs_module_text("Ellipsize.Start"), PANGO_ELLIPSIZE_START);
	obs_property_list_add_int(prop, obs_module_text("Ellipsize.Middle"), PANGO_ELLIPSIZE_MIDDLE);
	obs_property_list_add_int(prop, obs_module_text("Ellipsize.End"), PANGO_ELLIPSIZE_END);

	obs_properties_add_int(props, "spacing", obs_module_text("Line spacing"), -65536, +65536, 1);

	// TODO: vertical

	prop = obs_properties_add_bool(props, "outline", obs_module_text("Outline"));
	obs_property_set_modified_callback(prop, tp_prop_outline_changed);
	tp_data_add_color(props, "outline_color", obs_module_text("Outline color"));
	obs_properties_add_int(props, "outline_width", obs_module_text("Outline width"), 0, 65536, 1);
	obs_properties_add_int(props, "outline_blur", obs_module_text("Outline blur"), 0, 65536, 1);
	obs_properties_add_bool(props, "outline_blur_gaussian", obs_module_text("Outline blur with gaussian function"));
	prop = obs_properties_add_list(props, "outline_shape", obs_module_text("Outline shape"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Outline.Round"), OUTLINE_ROUND);
	obs_property_list_add_int(prop, obs_module_text("Outline.Bevel"), OUTLINE_BEVEL);
	obs_property_list_add_int(prop, obs_module_text("Outline.Rectangle"), OUTLINE_RECT);
	obs_property_list_add_int(prop, obs_module_text("Outline.Sharp"), OUTLINE_SHARP);

	prop = obs_properties_add_bool(props, "shadow", obs_module_text("Shadow"));
	obs_property_set_modified_callback(prop, tp_prop_shadow_changed);
	tp_data_add_color(props, "shadow_color", obs_module_text("Shadow color"));
	obs_properties_add_int(props, "shadow_x", obs_module_text("Shadow offset x"), -65536, 65536, 1);
	obs_properties_add_int(props, "shadow_y", obs_module_text("Shadow offset y"), -65536, 65536, 1);

	obs_properties_add_bool(props, "obs_output_caption_stream", obs_module_text("Send captions to stream"));

	return props;
}

static void catpion_update(void *data, obs_data_t *settings)
{
	struct obs_audio_caption_src *acs = data;
	check_cur_session(acs);
	if(acs->session != NULL){
		acs->lg.to_stream = obs_data_get_bool(settings, "obs_output_caption_stream");
	}

	uint32_t new_node_serial = obs_data_get_int(settings, "TargetId");

	pw_thread_loop_lock(acs->pw.thread_loop);

	if ((acs->default_info.autoconnect = new_node_serial == PW_ID_ANY)) {
		if (acs->default_info.node_serial != SPA_ID_INVALID) {
			start_streaming(acs, get_node_by_serial(acs, acs->default_info.node_serial));
		}
	} else {
		struct target_node *new_node = get_node_by_serial(acs, new_node_serial);
		if (new_node) {
			start_streaming(acs, new_node);

			obs_data_set_string(settings, "TargetName", acs->target_name.array);
		}
	}

	pw_thread_loop_unlock(acs->pw.thread_loop);

	tp_update(&acs->text_src, settings);
}

static void catpion_show(void *data)
{
	struct obs_audio_caption_src *acs = data;

	pw_thread_loop_lock(acs->pw.thread_loop);
	pw_stream_set_active(acs->pw.audio.stream, true);
	pw_thread_loop_unlock(acs->pw.thread_loop);
}

static void catpion_hide(void *data)
{
	struct obs_audio_caption_src *acs = data;
	pw_thread_loop_lock(acs->pw.thread_loop);
	pw_stream_set_active(acs->pw.audio.stream, false);
	pw_thread_loop_unlock(acs->pw.thread_loop);
}

static void catpion_destroy(void *data)
{
	struct obs_audio_caption_src *acs = data;

	pw_thread_loop_lock(acs->pw.thread_loop);

	obs_pw_audio_proxy_list_clear(&acs->targets);

	if (acs->default_info.metadata.proxy) {
		pw_proxy_destroy(acs->default_info.metadata.proxy);
	}

	obs_pw_audio_instance_destroy(&acs->pw);

	dstr_free(&acs->default_info.name);
	dstr_free(&acs->target_name);

	tp_thread_end(&acs->text_src);

	tp_config_destroy_member(&acs->text_src.config);

	if (acs->text_src.textures)
		free_texture(acs->text_src.textures);
	if (acs->text_src.tex_new)
		free_texture(acs->text_src.tex_new);

	pthread_mutex_destroy(&acs->text_src.tex_mutex);
	pthread_mutex_destroy(&acs->text_src.config_mutex);

	release_session(acs);
	bfree(acs);
}

static uint32_t caption_get_width(void *data)
{
	struct obs_audio_caption_src *acs = data;
	struct tp_source *src = &acs->text_src;

	uint32_t w = 0;
	struct tp_texture *t = src->textures;
	while (t) {
		if (w < t->width)
			w = t->width;
		t = t->next;
	}

	return w;
}

static uint32_t caption_get_height(void *data)
{
	struct obs_audio_caption_src *acs = data;
	struct tp_source *src = &acs->text_src;

	uint32_t h = 0;
	struct tp_texture *t = src->textures;
	while (t) {
		if (h < t->height)
			h = t->height;
		t = t->next;
	}

	return h;
}

static void tp_surface_to_texture(struct tp_texture *t)
{
	if (t->surface && !t->tex) {
		const uint8_t *surface = t->surface;
		t->tex = gs_texture_create(t->width, t->height, GS_BGRA, 1, &surface, 0);
	}
}

static void caption_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct obs_audio_caption_src *acs = data;
	struct tp_source *src = &acs->text_src;
	if (!textalpha_effect)
		return;

	obs_enter_graphics();
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	const int w = caption_get_width(data);
	const int h = caption_get_height(data);
	int xoff = 0, yoff = 0;

	for (struct tp_texture *t = src->textures; t; t = t->next) {
		if (!t->width || !t->height)
			continue;
		tp_surface_to_texture(t);
		int y0 = 0;
		int y1 = t->height;
		gs_effect_set_texture(gs_effect_get_param_by_name(textalpha_effect, "image"), t->tex);
		//gs_effect_set_float(gs_effect_get_param_by_name(textalpha_effect, "alpha"), t->fade_alpha / 255.f);
		while (gs_effect_loop(textalpha_effect, "Draw")) {
			gs_draw_sprite_subregion(t->tex, 0, 0, y0, t->width, y1);
		}
	}
	gs_blend_state_pop();
	obs_leave_graphics();
}

static inline void tp_load_new_texture(struct tp_source *src, uint64_t lastframe_ns)
{
	if (src->tex_new) {
		// new texture arrived

		struct tp_texture *tn = src->tex_new;
		src->tex_new = tn->next;
		tn->next = NULL;

		if (tn->config_updated) {
			// A texture with updated config is arrived.
			if (src->textures) {
				free_texture(src->textures);
				src->textures = NULL;
			}
		}

		if (tn->surface) {
			// if non-blank texture
			if (src->textures) {
				free_texture(src->textures);
				src->textures = NULL;
			}
		}

		src->textures = pushback_texture(src->textures, tn);
	}
}

static struct tp_texture *tp_pop_old_textures(struct tp_texture *t, uint64_t now_ns, const struct tp_source *src)
{
	if (!t)
		return NULL;

	bool deprecated = false;
	if (t->next)
		deprecated = true;


	if (deprecated) {
		t = popfront_texture(t);
		return tp_pop_old_textures(t, now_ns, src);
	}

	t->next = tp_pop_old_textures(t->next, now_ns, src);
	return t;
}

static void caption_tick(void *data, float seconds)
{
	struct obs_audio_caption_src *acs = data;
	struct tp_source *src = &acs->text_src;

	uint64_t now_ns = os_gettime_ns();
	uint64_t lastframe_ns = now_ns - (uint64_t)(seconds * 1e9);

	src->textures = tp_pop_old_textures(src->textures, now_ns, src);

	if (os_atomic_load_bool(&src->text_updating)) {
		// early notification for the new non-blank texture from the thread
		os_atomic_set_bool(&src->text_updating, false);
	}

	if (pthread_mutex_trylock(&src->tex_mutex) == 0) {
		tp_load_new_texture(src, lastframe_ns);
		pthread_mutex_unlock(&src->tex_mutex);
	}

	src->textures = tp_pop_old_textures(src->textures, now_ns, src);
}

const struct obs_source_info catpion_audio_input = {
	.id = "catpion_audio_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = catpion_audio_input_name,
	.create = catpion_audio_input_create,
	.get_defaults = catpion_defaults,
	.get_properties = catpion_properties,
	.update = catpion_update,
	.get_width = caption_get_width,
	.get_height = caption_get_height,
	.video_render = caption_render,
	.video_tick = caption_tick,
	.show = catpion_show,
	.hide = catpion_hide,
	.destroy = catpion_destroy,
	.icon_type = OBS_ICON_TYPE_TEXT,
};

bool obs_module_load(void)
{
	pw_init(NULL, NULL);

    aam_api_init(APRIL_VERSION);
	InitCatpionUI();

	obs_register_source(&catpion_audio_input);

	return true;
}

void obs_module_unload(void)
{
#if PW_CHECK_VERSION(0, 3, 49)
	pw_deinit();
#endif

	if (textalpha_effect) {
		gs_effect_destroy(textalpha_effect);
		textalpha_effect = NULL;
	}

	blog(LOG_INFO, "[catpion] plugin unloaded");
}