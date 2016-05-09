#ifndef __MEDIA_SET_PARSER_H__
#define __MEDIA_SET_PARSER_H__

// includes
#include "media_set.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	media_sequence_t* sequence;
	media_range_t* range;
	int64_t sequence_offset;
	uint32_t duration;
	media_clip_source_t* sources_head;
	media_clip_source_t* mapped_sources_head;
	struct media_clip_dynamic_s* dynamic_clips_head;
} media_filter_parse_context_t;

// main functions
vod_status_t media_set_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

vod_status_t media_set_parse_json(
	request_context_t* request_context,
	u_char* string,
	request_params_t* request_params,
	struct segmenter_conf_s* segmenter,
	vod_str_t* uri,
	bool_t parse_all_clips,
	media_set_t* result);

vod_status_t media_set_map_source(
	request_context_t* request_context,
	u_char* string,
	media_clip_source_t* source);

// filter utility functions
vod_status_t media_set_parse_null_term_string(
	void* ctx, 
	vod_json_value_t* value, 
	void* dest);

vod_status_t media_set_parse_filter_sources(
	void* ctx,
	vod_json_value_t* value,
	void* dest);

vod_status_t media_set_parse_clip(
	void* ctx,
	vod_json_object_t* element,
	media_clip_t* parent,
	media_clip_t** result);

#endif //__MEDIA_SET_PARSER_H__
