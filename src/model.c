#include "model.h"

#include <obs-module.h>

struct model_src {
	size_t u; //usage
	AprilASRModel m; //model
};

struct model_src models[MAX_MODELS] = {0};
size_t cur_model = 0;

void ModelNew(const char*input_model) {
	size_t next_model = (cur_model + 1) % MAX_MODELS;
	size_t old_model = cur_model;

	// create model on next id
	models[next_model].u = 0;
	models[next_model].m = aam_create_model(input_model);
	if(models[next_model].m == NULL){
		blog(LOG_INFO, "[catpion] Loading model %s failed!", input_model);
		return;
	}
	else {
		blog(LOG_INFO, "[catpion] Model %d name: %s", next_model, aam_get_name(models[next_model].m));
		blog(LOG_INFO, "[catpion] Model %d desc: %s", next_model, aam_get_description(models[next_model].m));
		blog(LOG_INFO, "[catpion] Model %d lang: %s", next_model, aam_get_language(models[next_model].m));
		blog(LOG_INFO, "[catpion] Model %d samplerate: %ld", next_model, aam_get_sample_rate(models[next_model].m));
	}

	// change current model id
	cur_model = next_model;
}

void ModelDelete() {
	if(models[cur_model].m == NULL) return;
	cur_model = (cur_model + 1) % MAX_MODELS;
}

size_t ModelCurID(){
	return cur_model;
}

AprilASRModel ModelGet(size_t id){
	if(id < 0 || id >= MAX_MODELS) return NULL;
	if(models[id].m == NULL) return NULL;
	return models[id].m;
}

void ModelTake(size_t id){
	if(models[id].m == NULL) return;
	++models[id].u;
}

void ModelRelease(size_t id) {
	if(models[id].m == NULL) return;
	if(--models[id].u <= 0)
	{
		aam_free(models[id].m);
		models[id].m = NULL;
		models[id].u = 0;
		blog(LOG_INFO, "[catpion] Unloaded model %d", id);
	}
}
