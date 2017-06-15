#ifndef __WEBVTT_BUILDER_H__
#define __WEBVTT_BUILDER_H__

// includes
#include "../media_set.h"

// constants
#define WEBVTT_TIMESCALE (1000)

// functions
vod_status_t webvtt_builder_build(
	request_context_t* request_context,
	media_set_t* media_set,
	bool_t clip_relative_timestamps,
	vod_str_t* result);

#endif //__WEBVTT_BUILDER_H__
