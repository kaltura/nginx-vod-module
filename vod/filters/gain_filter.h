#ifndef __GAIN_FILTER_H__
#define __GAIN_FILTER_H__

// includes
#include "../json_parser.h"

// functions
vod_status_t gain_filter_parse(
	void* context,
	vod_json_object_t* element,
	void** result);

vod_status_t gain_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __GAIN_FILTER_H__
