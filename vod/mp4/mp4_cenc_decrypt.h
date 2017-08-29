#ifndef __MP4_CENC_DECRYPT_H__
#define __MP4_CENC_DECRYPT_H__

// includes
#include "mp4_parser.h"

// globals
extern frames_source_t mp4_cenc_decrypt_frames_source;

// functions
vod_status_t mp4_cenc_decrypt_init(
	request_context_t* request_context,
	frames_source_t* frames_source,
	void* frames_source_context,
	u_char* key,
	media_encryption_t* encryption,
	void** result);

u_char* mp4_cenc_decrypt_get_key(void* context);

void mp4_cenc_decrypt_get_original_source(
	void* ctx,
	frames_source_t** frames_source,
	void** frames_source_context);

#endif //__MP4_CENC_DECRYPT_H__
