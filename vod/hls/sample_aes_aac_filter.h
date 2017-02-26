#ifndef __SAMPLE_AES_AAC_FILTER_H__
#define __SAMPLE_AES_AAC_FILTER_H__

// include
#include "media_filter.h"

// functions
vod_status_t sample_aes_aac_filter_init(
	void** context,
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	u_char* key,
	u_char* iv);

extern const media_filter_t sample_aes_aac;

#endif // __SAMPLE_AES_AAC_FILTER_H__
