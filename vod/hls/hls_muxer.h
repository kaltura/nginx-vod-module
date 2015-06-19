#ifndef __HLS_MUXER_H__
#define __HLS_MUXER_H__

// includes
#include "mp4_to_annexb_filter.h"
#include "adts_encoder_filter.h"
#include "mpegts_encoder_filter.h"
#include "buffer_filter.h"
#include "../mp4/mp4_parser.h"
#include "../read_cache.h"
#include "../segmenter.h"

// typedefs
typedef void(*hls_get_iframe_positions_callback_t)(
	void* context, 
	uint32_t segment_index, 
	uint32_t frame_duration, 
	uint32_t frame_start, 
	uint32_t frame_size);

typedef struct {
	int media_type;
	uint32_t stream_index;
	uint32_t timescale;
	uint32_t frames_file_index;
	
	// input frames
	input_frame_t* first_frame;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;

	// time offsets
	uint64_t first_frame_time_offset;
	uint64_t next_frame_time_offset;
	uint64_t next_frame_dts;
	uint64_t segment_limit;		// used only for iframes
	int32_t clip_from_frame_offset;
	
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
} hls_muxer_stream_state_t;

typedef struct {
	request_context_t* request_context;

	// fixed
	hls_muxer_stream_state_t* first_stream;
	hls_muxer_stream_state_t* last_stream;
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
	uint32_t cur_file_index;
} hls_muxer_state_t;

// functions
vod_status_t hls_muxer_init(
	hls_muxer_state_t* state,
	request_context_t* request_context,
	uint32_t segment_index,
	mpeg_metadata_t* mpeg_metadata,
	read_cache_state_t* read_cache_state,
	write_callback_t write_callback,
	void* write_context,
	bool_t* simulation_supported);

vod_status_t hls_muxer_process(hls_muxer_state_t* state);

vod_status_t hls_muxer_simulate_get_iframes(
	hls_muxer_state_t* state,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	hls_get_iframe_positions_callback_t callback,
	void* context);

uint32_t hls_muxer_simulate_get_segment_size(hls_muxer_state_t* state);

void hls_muxer_simulation_reset(hls_muxer_state_t* state);

#endif // __HLS_MUXER_H__
