#ifndef __ADTS_ENCODER_FILTER_H__
#define __ADTS_ENCODER_FILTER_H__

// includes
#include "hls_encryption.h"
#include "bit_fields.h"
#include "media_filter.h"
#include "../media_format.h"
#include "../common.h"

// functions
vod_status_t adts_encoder_init(
	media_filter_t* filter,
	media_filter_context_t* context);

vod_status_t adts_encoder_set_media_info(
	media_filter_context_t* context,
	media_info_t* media_info);

#endif // __ADTS_ENCODER_FILTER_H__
