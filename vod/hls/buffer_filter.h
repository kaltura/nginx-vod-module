#ifndef __BUFFER_FILTER_H__
#define __BUFFER_FILTER_H__

// include
#include "media_filter.h"

// functions
vod_status_t buffer_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	bool_t align_frames,
	uint32_t size);

vod_status_t buffer_filter_force_flush(
	media_filter_context_t* context, 
	bool_t last_stream_frame);

bool_t buffer_filter_get_dts(
	media_filter_context_t* context, 
	uint64_t* dts);


void buffer_filter_simulated_force_flush(
	media_filter_context_t* context, 
	bool_t last_stream_frame);

#endif // __BUFFER_FILTER_H__
