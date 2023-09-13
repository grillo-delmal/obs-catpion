#pragma once
#include <april_api.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_MODELS 5

void ModelNew(const char* input_model);
void ModelDelete();
size_t ModelCurID();
AprilASRModel ModelGet(size_t id);
void ModelTake(size_t id);
void ModelRelease(size_t id);

#ifdef __cplusplus
}
#endif
