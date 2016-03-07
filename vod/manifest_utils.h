#ifndef __MANIFEST_UTILS_H__
#define __MANIFEST_UTILS_H__

// includes
#include "media_set.h"

// functions
vod_status_t manifest_utils_build_request_params_string(
	request_context_t* request_context,
	uint32_t* has_tracks,
	uint32_t segment_index,
	uint32_t sequence_index,
	uint32_t* tracks_mask,
	vod_str_t* result);

#endif //__MANIFEST_UTILS_H__
