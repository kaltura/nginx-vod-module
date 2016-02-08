#ifndef __HLS_MUXER_H__
#define __HLS_MUXER_H__

// includes
#include "mp4_to_annexb_filter.h"
#include "adts_encoder_filter.h"
#include "mpegts_encoder_filter.h"
#include "buffer_filter.h"
#include "aes_cbc_encrypt.h"
#include "../media_format.h"
#include "../segmenter.h"

// typedefs
typedef void(*hls_get_iframe_positions_callback_t)(
	void* context, 
	uint32_t segment_index, 
	uint32_t frame_duration, 
	uint32_t frame_start, 
	uint32_t frame_size);

typedef struct {
	bool_t interleave_frames;
	bool_t align_frames;
} hls_muxer_conf_t;

typedef struct {
	int media_type;
	uint32_t timescale;
	
	// input frames
	frame_list_part_t* first_frame_part;
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;

	// time offsets
	uint64_t clip_start_time;
	uint64_t first_frame_time_offset;
	uint64_t next_frame_time_offset;
	uint64_t next_frame_dts;
	int32_t clip_from_frame_offset;

	// iframes simulation only
	uint64_t segment_limit;
	bool_t is_first_segment_frame;
	uint32_t prev_key_frame;
	uint64_t prev_frame_pts;

	// top filter
	const media_filter_t* top_filter;
	void* top_filter_context;
	
	// buffer
	buffer_filter_t* buffer_state;

	// mpegts
	mpegts_encoder_state_t mpegts_encoder_state;
} hls_muxer_stream_state_t;

typedef struct {
	request_context_t* request_context;

	// fixed
	hls_muxer_stream_state_t* first_stream;
	hls_muxer_stream_state_t* last_stream;
	uint32_t video_duration;

	// child states
	write_buffer_queue_t queue;
	aes_cbc_encrypt_context_t* encrypted_write_context;
	
	// cur sequence state
	media_clip_filtered_t* clips_start;
	media_clip_filtered_t* clips_end;
	media_clip_filtered_t* cur_clip;
	bool_t use_discontinuity;

	// cur frame state
	input_frame_t* cur_frame;
	bool_t last_stream_frame;
	const media_filter_t* cur_writer;
	void* cur_writer_context;
	int cache_slot_id;
	frames_source_t* frames_source;
	void* frames_source_context;
	bool_t first_time;
} hls_muxer_state_t;

// functions
vod_status_t hls_muxer_init_segment(
	request_context_t* request_context,
	hls_muxer_conf_t* conf,
	hls_encryption_params_t* encryption_params,
	uint32_t segment_index,
	media_set_t* media_set,
	write_callback_t write_callback,
	void* write_context,
	size_t* response_size,
	vod_str_t* response_header,
	hls_muxer_state_t** processor_state);

vod_status_t hls_muxer_process(hls_muxer_state_t* state);

vod_status_t hls_muxer_simulate_get_iframes(
	request_context_t* request_context,
	segment_durations_t* segment_durations,
	hls_muxer_conf_t* muxer_conf,
	hls_encryption_params_t* encryption_params,
	media_set_t* media_set,
	hls_get_iframe_positions_callback_t callback,
	void* context);

#endif // __HLS_MUXER_H__
