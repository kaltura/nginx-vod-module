#ifndef __MP4_TO_ANNEXB_FILTER_H__
#define __MP4_TO_ANNEXB_FILTER_H__

// includes
#include "media_filter.h"

// typedefs
typedef struct {
	// input data
	request_context_t* request_context;
	const media_filter_t* next_filter;
	void* next_filter_context;

	// data parsed from extra data
	uint32_t nal_packet_size_length;
	const u_char* sps_pps;
	uint32_t sps_pps_size;
	
	// state
	int cur_state;
	bool_t first_idr;
	bool_t first_frame_packet;
	bool_t key_frame;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;
	int32_t frame_size_left;
} mp4_to_annexb_state_t;

// globals
extern const media_filter_t mp4_to_annexb;

// functions
vod_status_t mp4_to_annexb_init(
	mp4_to_annexb_state_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	const u_char* extra_data, 
	uint32_t extra_data_size,
	uint32_t nal_packet_size_length);

bool_t mp4_to_annexb_simulation_supported(mp4_to_annexb_state_t* state);

#endif // __MP4_TO_ANNEXB_FILTER_H__
