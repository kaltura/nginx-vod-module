#ifndef __FRAME_ENCRYPT_FILTER_H__
#define __FRAME_ENCRYPT_FILTER_H__

// include
#include "media_filter.h"
#include "hls_encryption.h"

// functions
vod_status_t frame_encrypt_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	hls_encryption_params_t* encryption_params);

void frame_encrypt_start_sub_frame(
	media_filter_context_t* context,
	uint32_t size);

#endif // __FRAME_ENCRYPT_FILTER_H__
