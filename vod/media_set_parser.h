#ifndef __MEDIA_SET_PARSER_H__
#define __MEDIA_SET_PARSER_H__

// includes
#include "media_set.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	media_sequence_t* sequence;
	media_range_t* range;
	int64_t clip_time;
	uint32_t clip_from;
	uint32_t duration;
	media_clip_source_t* sources_head;
	media_clip_source_t* mapped_sources_head;
	media_clip_source_t* generators_head;
	struct media_clip_dynamic_s* dynamic_clips_head;
	media_notification_t* notifications_head;
} media_filter_parse_context_t;

// main functions
vod_status_t media_set_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

vod_status_t media_set_parse_json(
	request_context_t* request_context,
	u_char* string,
	u_char* override,
	request_params_t* request_params,
	struct segmenter_conf_s* segmenter,
	media_clip_source_t* source,
	int request_flags,
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

vod_status_t media_set_parse_notifications(
	request_context_t* request_context,
	vod_json_array_t* array,
	int64_t min_offset,
	int64_t max_offset,
	media_notification_t** result);

#endif //__MEDIA_SET_PARSER_H__
