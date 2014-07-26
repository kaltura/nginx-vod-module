#ifndef __MUXER_H__
#define __MUXER_H__

// includes
#include "mp4_to_annexb_filter.h"
#include "adts_encoder_filter.h"
#include "mpegts_encoder_filter.h"
#include "buffer_filter.h"
#include "mp4_parser.h"
#include "read_cache.h"

// typedefs
typedef void(*get_iframe_positions_callback_t)(
	void* context, 
	int segment_index, 
	uint32_t frame_duration, 
	uint32_t frame_start, 
	uint32_t frame_size);

typedef struct {
	int media_type;
	uint32_t stream_index;
	
	// input frames
	input_frame_t* first_frame;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	
	// frame offsets
	uint64_t* first_frame_offset;
	uint64_t* cur_frame_offset;
	
	// output frame
	output_frame_t output_frame;
	unsigned cc;
	
	// top filter
	const media_filter_t* top_filter;
	void* top_filter_context;
	
	// buffer
	buffer_filter_t* buffer_state;
} muxer_stream_state_t;

typedef struct {
	request_context_t* request_context;

	// fixed
	muxer_stream_state_t* first_stream;
	muxer_stream_state_t* last_stream;
	uint32_t video_duration;

	// child states
	read_cache_state_t* read_cache_state;
	mpegts_encoder_state_t mpegts_encoder_state;
	
	// cur frame state
	input_frame_t* cur_frame;
	uint64_t cur_frame_offset;
	const media_filter_t* cur_writer;
	void* cur_writer_context;
	uint32_t cur_frame_pos;
	int cache_slot_id;
} muxer_state_t;

// functions
vod_status_t muxer_init(
	muxer_state_t* state,
	request_context_t* request_context,
	int segment_index,
	mpeg_metadata_t* mpeg_metadata,
	read_cache_state_t* read_cache_state,
	write_callback_t write_callback,
	void* write_context,
	bool_t* simulation_supported);

vod_status_t muxer_process(muxer_state_t* state, uint64_t* required_offset);

void muxer_simulate_get_iframes(muxer_state_t* state, uint32_t segment_duration, get_iframe_positions_callback_t callback, void* context);

uint32_t muxer_simulate_get_segment_size(muxer_state_t* state);

void muxer_simulation_reset(muxer_state_t* state);

#endif // __MUXER_H__
