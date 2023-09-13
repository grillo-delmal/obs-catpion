/* pipewire-audio.c
 *
 * Copyright 2022 Dimitris Papaioannou <dimtpap@protonmail.com>
 * Modified by Grillo del Mal (2023)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
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

#include "pipewire-audio.h"

#include <obs-module.h>
#include <util/platform.h>

#include <spa/utils/json.h>

#include "catpion.h"

/* Utilities */
bool json_object_find(const char *obj, const char *key, char *value, size_t len)
{
	/* From PipeWire's source */

	struct spa_json it[2];
	const char *v;
	char k[128];

	spa_json_init(&it[0], obj, strlen(obj));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0) {
		return false;
	}

	while (spa_json_get_string(&it[1], k, sizeof(k)) > 0) {
		if (spa_streq(k, key)) {
			if (spa_json_get_string(&it[1], value, len) > 0) {
				return true;
			}
		} else if (spa_json_next(&it[1], &v) <= 0) {
			break;
		}
	}
	return false;
}
/* ------------------------------------------------- */

/* PipeWire stream wrapper */
static void on_process_cb(void *data)
{
	uint64_t now = os_gettime_ns();

	struct obs_pw_audio_stream *s = data;

	struct pw_buffer *b = pw_stream_dequeue_buffer(s->stream);

	if (!b) {
		if(s->acs->session) {
	        aas_flush(s->acs->session);
		}
		return;
	}

	struct spa_buffer *buf = b->buffer;

    if (buf->datas[0].data == NULL){
		if(s->acs->session) {
	        aas_flush(s->acs->session);
		}
		goto queue;
    }

	if(s->acs->session) {
	    uint32_t n_channels, n_samples;

		n_channels = s->format.info.raw.channels;
		n_samples = buf->datas[0].chunk->size / sizeof(short);

		aas_feed_pcm16(s->acs->session, (short *)buf->datas[0].data, n_samples);
	}

queue:
	pw_stream_queue_buffer(s->stream, b);
}

static void on_state_changed_cb(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	UNUSED_PARAMETER(old);

	struct obs_pw_audio_stream *s = data;

	blog(LOG_DEBUG, "[catpion] Stream %p state: \"%s\" (error: %s)", s->stream, pw_stream_state_as_string(state),
		 error ? error : "none");
}

static void on_param_changed_cb(void *data, uint32_t id, const struct spa_pod *param)
{
	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	struct obs_pw_audio_stream *s = data;

	/* only accept raw audio */
    if (s->format.media_type != SPA_MEDIA_TYPE_audio ||
            s->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    /* call a helper function to parse the format for us. */
    spa_format_audio_raw_parse(param, &s->format.info.raw);

    blog(LOG_INFO, "[catpion] capturing rate:%d channels:%d",
		s->format.info.raw.rate, s->format.info.raw.channels);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process_cb,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
};

int obs_pw_audio_stream_connect(
	struct obs_pw_audio_stream *s, uint32_t target_serial, uint32_t audio_channels, 
	uint32_t model_sample_rate)
{
	uint8_t buffer[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	params[0] = spa_format_audio_raw_build(
		&b, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_S16_LE,
			.channels = audio_channels,
			.rate = model_sample_rate));

	struct pw_properties *stream_props = pw_properties_new(NULL, NULL);
	pw_properties_setf(stream_props, PW_KEY_TARGET_OBJECT, "%u", target_serial);
	pw_stream_update_properties(s->stream, &stream_props->dict);
	pw_properties_free(stream_props);

	return pw_stream_connect(
		s->stream, 
		PW_DIRECTION_INPUT, 
		PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | 
		PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS | 
		PW_STREAM_FLAG_DONT_RECONNECT,
		params, 1);
}
/* ------------------------------------------------- */

/* Common PipeWire components */
static void on_core_done_cb(void *data, uint32_t id, int seq)
{
	struct obs_pw_audio_instance *pw = data;

	if (id == PW_ID_CORE && pw->seq == seq) {
		pw_thread_loop_signal(pw->thread_loop, false);
	}
}

static void on_core_error_cb(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct obs_pw_audio_instance *pw = data;

	blog(LOG_ERROR, "[catpion] Error id:%u seq:%d res:%d :%s", id, seq, res, message);

	pw_thread_loop_signal(pw->thread_loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

bool obs_pw_audio_instance_init(struct obs_pw_audio_instance *pw, const struct pw_registry_events *registry_events,
								void *registry_cb_data, bool stream_capture_sink, bool stream_want_driver,
								struct obs_audio_caption_src *acs)
{
	pw->thread_loop = pw_thread_loop_new("PipeWire thread loop", NULL);
	pw->context = pw_context_new(pw_thread_loop_get_loop(pw->thread_loop), NULL, 0);

	pw_thread_loop_lock(pw->thread_loop);

	if (pw_thread_loop_start(pw->thread_loop) < 0) {
		blog(LOG_WARNING, "[catpion] Error starting threaded mainloop");
		return false;
	}

	pw->core = pw_context_connect(pw->context, NULL, 0);
	if (!pw->core) {
		blog(LOG_WARNING, "[catpion] Error creating PipeWire core");
		return false;
	}

	pw_core_add_listener(pw->core, &pw->core_listener, &core_events, pw);

	pw->registry = pw_core_get_registry(pw->core, PW_VERSION_REGISTRY, 0);
	if (!pw->registry) {
		return false;
	}
	pw_registry_add_listener(pw->registry, &pw->registry_listener, registry_events, registry_cb_data);

	pw->audio.acs = acs;
	pw->audio.stream =
		pw_stream_new(
			pw->core, "OBS",
			pw_properties_new(
				PW_KEY_NODE_NAME, "OBS", 
				PW_KEY_NODE_DESCRIPTION, "OBS Audio Capture",
				PW_KEY_MEDIA_TYPE, "Audio", 
				PW_KEY_MEDIA_CATEGORY, "Capture", 
				PW_KEY_MEDIA_ROLE, "Production", 
				PW_KEY_NODE_WANT_DRIVER, stream_want_driver ? "true" : "false",
				PW_KEY_STREAM_CAPTURE_SINK, stream_capture_sink ? "true" : "false", 
				NULL));

	if (!pw->audio.stream) {
		blog(LOG_WARNING, "[catpion] Failed to create stream");
		return false;
	}
	blog(LOG_INFO, "[catpion] Created stream %p", pw->audio.stream);

	pw_stream_add_listener(pw->audio.stream, &pw->audio.stream_listener, &stream_events, &pw->audio);

	return true;
}

void obs_pw_audio_instance_destroy(struct obs_pw_audio_instance *pw)
{
	if (pw->audio.stream) {
		spa_hook_remove(&pw->audio.stream_listener);
		if (pw_stream_get_state(pw->audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
			pw_stream_disconnect(pw->audio.stream);
		}
		pw_stream_destroy(pw->audio.stream);
	}

	if (pw->registry) {
		spa_hook_remove(&pw->registry_listener);
		spa_zero(pw->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)pw->registry);
	}

	pw_thread_loop_unlock(pw->thread_loop);
	pw_thread_loop_stop(pw->thread_loop);

	if (pw->core) {
		spa_hook_remove(&pw->core_listener);
		spa_zero(pw->core_listener);
		pw_core_disconnect(pw->core);
	}

	if (pw->context) {
		pw_context_destroy(pw->context);
	}

	pw_thread_loop_destroy(pw->thread_loop);
}

void obs_pw_audio_instance_sync(struct obs_pw_audio_instance *pw)
{
	pw->seq = pw_core_sync(pw->core, PW_ID_CORE, pw->seq);
}
/* ------------------------------------------------- */

/* PipeWire metadata */
static int on_metadata_property_cb(void *data, uint32_t id, const char *key, const char *type, const char *value)
{
	UNUSED_PARAMETER(type);

	struct obs_pw_audio_default_node_metadata *metadata = data;

	if (id == PW_ID_CORE && key && value &&
		strcmp(key, metadata->wants_sink ? "default.audio.sink" : "default.audio.source") == 0) {
		char val[128];
		if (json_object_find(value, "name", val, sizeof(val)) && *val) {
			metadata->default_node_callback(metadata->data, val);
		}
	}

	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_metadata_property_cb,
};

static void on_metadata_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_default_node_metadata *metadata = data;
	pw_proxy_destroy(metadata->proxy);
}

static void on_metadata_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_default_node_metadata *metadata = data;

	spa_hook_remove(&metadata->metadata_listener);
	spa_hook_remove(&metadata->proxy_listener);
	spa_zero(metadata->metadata_listener);
	spa_zero(metadata->proxy_listener);

	metadata->proxy = NULL;
}

static const struct pw_proxy_events metadata_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_metadata_proxy_removed_cb,
	.destroy = on_metadata_proxy_destroy_cb,
};

bool obs_pw_audio_default_node_metadata_listen(struct obs_pw_audio_default_node_metadata *metadata,
											   struct obs_pw_audio_instance *pw, uint32_t global_id, bool wants_sink,
											   void (*default_node_callback)(void *data, const char *name), void *data)
{
	if (metadata->proxy) {
		pw_proxy_destroy(metadata->proxy);
	}

	struct pw_proxy *metadata_proxy =
		pw_registry_bind(pw->registry, global_id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
	if (!metadata_proxy) {
		return false;
	}

	metadata->proxy = metadata_proxy;

	metadata->wants_sink = wants_sink;

	metadata->default_node_callback = default_node_callback;
	metadata->data = data;

	pw_proxy_add_object_listener(metadata->proxy, &metadata->metadata_listener, &metadata_events, metadata);
	pw_proxy_add_listener(metadata->proxy, &metadata->proxy_listener, &metadata_proxy_events, metadata);

	return true;
}
/* ------------------------------------------------- */

/* Proxied objects */
static void on_proxy_bound_cb(void *data, uint32_t global_id)
{
	struct obs_pw_audio_proxied_object *obj = data;
	if (obj->bound_callback) {
		obj->bound_callback(pw_proxy_get_user_data(obj->proxy), global_id);
	}
}

static void on_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_proxied_object *obj = data;
	pw_proxy_destroy(obj->proxy);
}

static void on_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_proxied_object *obj = data;
	spa_hook_remove(&obj->proxy_listener);

	spa_list_remove(&obj->link);

	if (obj->destroy_callback) {
		obj->destroy_callback(pw_proxy_get_user_data(obj->proxy));
	}

	bfree(data);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.bound = on_proxy_bound_cb,
	.removed = on_proxy_removed_cb,
	.destroy = on_proxy_destroy_cb,
};

void obs_pw_audio_proxied_object_init(struct obs_pw_audio_proxied_object *obj, struct pw_proxy *proxy,
									  struct spa_list *list, void (*bound_callback)(void *data, uint32_t global_id),
									  void (*destroy_callback)(void *data))
{
	obj->proxy = proxy;
	obj->bound_callback = bound_callback;
	obj->destroy_callback = destroy_callback;

	spa_list_append(list, &obj->link);

	spa_zero(obj->proxy_listener);
	pw_proxy_add_listener(obj->proxy, &obj->proxy_listener, &proxy_events, obj);
}

void *obs_pw_audio_proxied_object_get_user_data(struct obs_pw_audio_proxied_object *obj)
{
	return pw_proxy_get_user_data(obj->proxy);
}

void obs_pw_audio_proxy_list_init(struct obs_pw_audio_proxy_list *list,
								  void (*bound_callback)(void *data, uint32_t global_id),
								  void (*destroy_callback)(void *data))
{
	spa_list_init(&list->list);

	list->bound_callback = bound_callback;
	list->destroy_callback = destroy_callback;
}

void obs_pw_audio_proxy_list_append(struct obs_pw_audio_proxy_list *list, struct pw_proxy *proxy)
{
	struct obs_pw_audio_proxied_object *obj = bmalloc(sizeof(struct obs_pw_audio_proxied_object));
	obs_pw_audio_proxied_object_init(obj, proxy, &list->list, list->bound_callback, list->destroy_callback);
}

void obs_pw_audio_proxy_list_clear(struct obs_pw_audio_proxy_list *list)
{
	struct obs_pw_audio_proxied_object *obj, *temp;
	spa_list_for_each_safe(obj, temp, &list->list, link)
	{
		pw_proxy_destroy(obj->proxy);
	}
}
/* ------------------------------------------------- */
