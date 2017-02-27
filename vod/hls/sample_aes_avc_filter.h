#ifndef __SAMPLE_AES_AVC_FILTER_H__
#define __SAMPLE_AES_AVC_FILTER_H__

// includes
#include "media_filter.h"

// functions
vod_status_t sample_aes_avc_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	u_char* key,
	u_char* iv);

vod_status_t sample_aes_avc_start_nal_unit(
	media_filter_context_t* context, 
	int unit_type, 
	uint32_t unit_size);

vod_status_t sample_aes_avc_filter_write_nal_body(
	media_filter_context_t* context, 
	const u_char* buffer, 
	uint32_t size);

#endif //__SAMPLE_AES_AVC_FILTER_H__
