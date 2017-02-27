#ifndef __SAMPLE_AES_AAC_FILTER_H__
#define __SAMPLE_AES_AAC_FILTER_H__

// include
#include "media_filter.h"
#include "hls_encryption.h"

// functions
vod_status_t sample_aes_aac_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	hls_encryption_params_t* encryption_params);

#endif // __SAMPLE_AES_AAC_FILTER_H__
