#ifndef __MP4_TO_ANNEXB_FILTER_H__
#define __MP4_TO_ANNEXB_FILTER_H__

// includes
#include "hls_encryption.h"
#include "media_filter.h"
#include "../media_format.h"

// typedefs
typedef struct {
	// input data
	request_context_t* request_context;
	const media_filter_t* next_filter;
	void* next_filter_context;

	// fixed
	media_filter_write_t body_write;
	void* body_write_context;
	uint8_t unit_type_mask;
	uint8_t aud_unit_type;
	const u_char* aud_nal_packet;
	uint32_t aud_nal_packet_size;

	// data parsed from extra data
	uint32_t nal_packet_size_length;
	const u_char* extra_data;
	uint32_t extra_data_size;
	
	// state
	int cur_state;
	bool_t first_frame_packet;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;
	int32_t frame_size_left;

	void* sample_aes_context;
} mp4_to_annexb_state_t;

// globals
extern const media_filter_t mp4_to_annexb;

// functions
vod_status_t mp4_to_annexb_init(
	mp4_to_annexb_state_t* state,
	request_context_t* request_context,
	hls_encryption_params_t* encryption_params,
	const media_filter_t* next_filter,
	void* next_filter_context);

vod_status_t mp4_to_annexb_set_media_info(
	mp4_to_annexb_state_t* state,
	media_info_t* media_info);

bool_t mp4_to_annexb_simulation_supported(media_info_t* media_info);

#endif // __MP4_TO_ANNEXB_FILTER_H__
