#ifndef __CONCAT_CLIP_H__
#define __CONCAT_CLIP_H__

// includes
#include "../json_parser.h"
#include "../media_clip.h"

// functions
vod_status_t concat_clip_parse(
	void* context,
	vod_json_object_t* element,
	void** result);

vod_status_t concat_clip_concat(
	request_context_t* request_context,
	media_clip_t* clip);

vod_status_t concat_clip_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __CONCAT_CLIP_H__
