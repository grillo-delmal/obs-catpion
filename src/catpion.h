#pragma once

#include <obs-module.h>

#include <util/dstr.h>
#include <april_api.h>

#include "pipewire-audio.h"
#include "obs-text-pthread.h"
#include "line-gen.h"

struct obs_audio_caption_src {
	obs_source_t *source;

	struct tp_source text_src;

	struct obs_pw_audio_instance pw;

	struct {
		struct obs_pw_audio_default_node_metadata metadata;
		bool autoconnect;
		uint32_t node_serial;
		struct dstr name;
	} default_info;

	struct obs_pw_audio_proxy_list targets;

	struct dstr target_name;
	uint32_t connected_serial;

    size_t model_id;
    size_t model_sample_rate;
    AprilASRSession session;
    struct line_generator lg;
};

void InitCatpionUI();
