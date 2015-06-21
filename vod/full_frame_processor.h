#ifndef __FULL_FRAME_PROCESSOR_H__
#define __FULL_FRAME_PROCESSOR_H__

// includes
#include "read_cache.h"
#include "mp4/mp4_parser.h"

// typedefs
typedef vod_status_t(*frame_callback_t)(void* context, input_frame_t* frame, u_char* buffer);

typedef struct {
	request_context_t* request_context;
	frame_callback_t frame_callback;
	void* frame_context;
	uint32_t frames_file_index;

	read_cache_state_t* read_cache_state;
	u_char* frame_buffer;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t* cur_frame_offset;
	uint32_t cur_frame_pos;
	bool_t first_time;
} full_frame_processor_t;

// functions
vod_status_t full_frame_processor_init(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	read_cache_state_t* read_cache_state,
	frame_callback_t frame_callback,
	void* frame_context,
	full_frame_processor_t** result);

vod_status_t full_frame_processor_process(full_frame_processor_t* state);

#endif // __FULL_FRAME_PROCESSOR_H__
