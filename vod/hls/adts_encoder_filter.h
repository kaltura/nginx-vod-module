#ifndef __ADTS_ENCODER_FILTER_H__
#define __ADTS_ENCODER_FILTER_H__

// includes
#include "hls_encryption.h"
#include "bit_fields.h"
#include "media_filter.h"
#include "../common.h"

// typedefs
typedef struct {
	// input
	const media_filter_t* next_filter;
	void* next_filter_context;

	// fixed
	u_char header[sizeof_adts_frame_header];
	media_filter_write_t body_write;
	void* body_write_context;

	// state
	void* sample_aes_context;
} adts_encoder_state_t;

// globals
extern const media_filter_t adts_encoder;

// functions
vod_status_t adts_encoder_init(
	adts_encoder_state_t* state, 
	request_context_t* request_context,
	hls_encryption_params_t* encryption_params,
	const media_filter_t* next_filter,
	void* next_filter_context,
	const u_char* extra_data, 
	uint32_t extra_data_size);

#endif // __ADTS_ENCODER_FILTER_H__
