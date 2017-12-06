#ifndef __DYNAMIC_CLIP_H__
#define __DYNAMIC_CLIP_H__

// includes
#include "../json_parser.h"
#include "../media_set.h"

// typedefs
struct media_clip_dynamic_s;
typedef struct media_clip_dynamic_s media_clip_dynamic_t;

// typedefs
struct media_clip_dynamic_s {
	media_clip_t base;
	vod_str_t id;
	struct media_sequence_s* sequence;
	media_range_t* range;
	int64_t clip_time;
	uint32_t duration;
	uint32_t clip_from;
	media_clip_dynamic_t* next;
};

// functions
vod_status_t dynamic_clip_parse(
	void* context,
	vod_json_object_t* element,
	void** result);

vod_status_t dynamic_clip_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

vod_status_t dynamic_clip_apply_mapping_json(
	media_clip_dynamic_t* clip,
	request_context_t* request_context,
	u_char* mapping,
	media_set_t* media_set);

vod_status_t dynamic_clip_get_mapping_string(
	request_context_t* request_context,
	media_clip_dynamic_t* dynamic_clips_head,
	vod_str_t* result);

vod_status_t dynamic_clip_apply_mapping_string(
	request_context_t* request_context,
	media_set_t* media_set,
	vod_str_t* mapping);

#endif // __DYNAMIC_CLIP_H__
