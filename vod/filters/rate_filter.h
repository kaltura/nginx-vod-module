#ifndef __RATE_FILTER_H__
#define __RATE_FILTER_H__

// includes
#include "../media_set.h"
#include "../json_parser.h"

// typedefs
typedef struct {
	media_clip_t base;
	vod_fraction_t rate;
} media_clip_rate_filter_t;

// functions
void rate_filter_scale_track_timestamps(
	media_track_t* track,
	uint32_t speed_num,
	uint32_t speed_denom);

vod_status_t rate_filter_parse(
	void* context,
	vod_json_object_t* element,
	void** result);

vod_status_t rate_filter_create_from_string(
	request_context_t* request_context,
	vod_str_t* str,
	media_clip_t* source,
	media_clip_rate_filter_t** result);

vod_status_t rate_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __RATE_FILTER_H__
