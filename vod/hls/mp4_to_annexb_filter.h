#ifndef __MP4_TO_ANNEXB_FILTER_H__
#define __MP4_TO_ANNEXB_FILTER_H__

// includes
#include "hls_encryption.h"
#include "media_filter.h"
#include "../media_format.h"

// functions
vod_status_t mp4_to_annexb_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	hls_encryption_params_t* encryption_params);

vod_status_t mp4_to_annexb_set_media_info(
	media_filter_context_t* context,
	media_info_t* media_info);

bool_t mp4_to_annexb_simulation_supported(
	media_info_t* media_info);

#endif // __MP4_TO_ANNEXB_FILTER_H__
