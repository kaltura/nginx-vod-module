#ifndef __CONCAT_FILTER_H__
#define __CONCAT_FILTER_H__

// includes
#include "../json_parser.h"
#include "../media_clip.h"

// functions
vod_status_t concat_filter_parse(
	void* context,
	vod_json_value_t* element,
	void** result);

vod_status_t concat_filter_concat(
	request_context_t* request_context,
	media_clip_t* clip);

vod_status_t concat_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __CONCAT_FILTER_H__
