#ifndef __MPEGTS_ENCODER_FILTER_H__
#define __MPEGTS_ENCODER_FILTER_H__

// includes
#include "media_filter.h"
#include "../write_buffer_queue.h"

// constants
#define MPEGTS_PACKET_SIZE (188)
#define HLS_DELAY (63000)			// 700 ms PCR delay
#define INITIAL_DTS (9090)
#define INITIAL_PCR (4590)

// typedefs
typedef struct {
	int media_type;
	unsigned pid;
	unsigned sid;
} mpegts_stream_info_t;

typedef struct {
	request_context_t* request_context;

	// stream info
	mpegts_stream_info_t stream_info;

	// options
	bool_t interleave_frames;
	bool_t align_frames;

	// buffer queue
	write_buffer_queue_t* queue;
	off_t send_queue_offset;
	off_t last_queue_offset;

	// packet state
	u_char* cur_packet_start;
	u_char* cur_packet_end;
	u_char* cur_pos;
	u_char* temp_packet;
	
	// frame state
	unsigned cc;
	u_char* cur_pes_size_ptr;
	uint32_t pes_bytes_written;
	uint32_t flushed_frame_bytes;
	uint32_t packet_bytes_left;
	uint32_t header_size;

	// simulation only
	uint32_t temp_packet_size;
	off_t cur_frame_start_pos;
	off_t cur_frame_end_pos;
	off_t last_frame_start_pos;
	off_t last_frame_end_pos;
} mpegts_encoder_state_t;

typedef struct {
	request_context_t* request_context;

	u_char* pmt_packet_start;
	u_char* pmt_packet_end;
	u_char* pmt_packet_pos;
	int cur_pid;
	unsigned cur_video_sid;
	unsigned cur_audio_sid;
} mpegts_encoder_init_streams_state_t;

// globals
extern const media_filter_t mpegts_encoder;

// functions
vod_status_t mpegts_encoder_init_streams(
	request_context_t* request_context,
	write_buffer_queue_t* queue,
	mpegts_encoder_init_streams_state_t* stream_state,
	uint32_t segment_index);

void mpegts_encoder_finalize_streams(
	mpegts_encoder_init_streams_state_t* stream_state);

vod_status_t mpegts_encoder_init(
	mpegts_encoder_state_t* state,
	mpegts_encoder_init_streams_state_t* stream_state,
	int media_type,
	request_context_t* request_context,
	write_buffer_queue_t* queue,
	bool_t interleave_frames,
	bool_t align_frames);

bool_t mpegts_encoder_is_new_packet(void* context, off_t* marker);

void mpegts_encoder_simulated_start_segment(write_buffer_queue_t* queue);

#endif // __MPEGTS_ENCODER_FILTER_H__
