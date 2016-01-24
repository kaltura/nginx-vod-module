#ifndef __MEDIA_SET_PARSER_H__
#define __MEDIA_SET_PARSER_H__

// includes
#include "media_set.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	media_sequence_t* sequence;
	media_range_t* range;
	uint64_t sequence_offset;
	uint32_t duration;
	vod_array_t sources;
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

// filter utility functions
vod_status_t media_set_parse_filter_sources(
	void* ctx,
	vod_json_value_t* value,
	void* dest);

vod_status_t media_set_parse_clip(
	void* ctx,
	vod_json_value_t* element,
	media_clip_t* parent,
	media_clip_t** result);

#endif //__MEDIA_SET_PARSER_H__
