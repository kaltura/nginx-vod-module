#ifndef __MIX_FILTER_H__
#define __MIX_FILTER_H__

// includes
#include "../json_parser.h"

// functions
vod_status_t mix_filter_parse(
	void* context,
	vod_json_object_t* element,
	void** result);

vod_status_t mix_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __MIX_FILTER_H__
