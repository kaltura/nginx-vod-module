#ifndef __TTML_BUILDER_H__
#define __TTML_BUILDER_H__

// includes
#include "../media_set.h"

// constants
#define TTML_TIMESCALE (1000)

// globals
vod_status_t ttml_build_mp4(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	uint32_t timescale,
	vod_str_t* result);

#endif //__TTML_BUILDER_H__
