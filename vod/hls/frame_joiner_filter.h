#ifndef __FRAME_JOINER_H__
#define __FRAME_JOINER_H__

// include
#include "media_filter.h"

// functions
vod_status_t frame_joiner_init(
	media_filter_t* filter,
	media_filter_context_t* context);

#endif // __FRAME_JOINER_H__
