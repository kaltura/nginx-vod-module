#ifndef __SAMPLE_AES_AAC_FILTER_H__
#define __SAMPLE_AES_AAC_FILTER_H__

// include
#include "media_filter.h"

// functions
vod_status_t sample_aes_aac_filter_init(
	void** context,
	request_context_t* request_context,
	media_filter_write_t write_callback,
	void* write_context,
	u_char* key,
	u_char* iv);

vod_status_t sample_aes_aac_start_frame(void* context, output_frame_t* frame);

vod_status_t sample_aes_aac_filter_write_frame_body(void* context, const u_char* buffer, uint32_t size);

#endif // __SAMPLE_AES_AAC_FILTER_H__
