#ifndef __SAMPLE_AES_AVC_FILTER_H__
#define __SAMPLE_AES_AVC_FILTER_H__

// includes
#include "media_filter.h"

// functions
vod_status_t sample_aes_avc_filter_init(
	void** context,
	request_context_t* request_context,
	media_filter_write_t write_callback,
	void* write_context,
	u_char* key,
	u_char* iv);

vod_status_t sample_aes_avc_start_nal_unit(void* context, int unit_type, uint32_t unit_size);

vod_status_t sample_aes_avc_filter_write_nal_body(void* context, const u_char* buffer, uint32_t size);

#endif //__SAMPLE_AES_AVC_FILTER_H__
