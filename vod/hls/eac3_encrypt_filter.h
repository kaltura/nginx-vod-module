#ifndef __EAC3_ENCRYPT_FILTER_H__
#define __EAC3_ENCRYPT_FILTER_H__

// include
#include "media_filter.h"
#include "hls_encryption.h"

// functions
vod_status_t eac3_encrypt_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context);

#endif // __EAC3_ENCRYPT_FILTER_H__
