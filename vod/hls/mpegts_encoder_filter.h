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
	request_context_t* request_context;

	// buffer queue
	write_buffer_queue_t queue;

	// packet state
	u_char* cur_packet_start;
	u_char* cur_packet_end;
	u_char* cur_pos;
	
	// frame state
	unsigned pid;
	unsigned* cc;
	bool_t last_stream_frame;
	unsigned cur_pes_header_size;
	u_char* cur_pes_size_ptr;
	uint32_t pes_bytes_written;
	uint32_t simulated_offset;		// simulated mode only
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
vod_status_t mpegts_encoder_init(
	mpegts_encoder_state_t* state, 
	request_context_t* request_context, 
	uint32_t segment_index,
	write_callback_t write_callback, 
	void* write_context);

vod_status_t mpegts_encoder_init_streams(mpegts_encoder_state_t* state, mpegts_encoder_init_streams_state_t* stream_state, uint32_t segment_index);
vod_status_t mpegts_encoder_add_stream(mpegts_encoder_init_streams_state_t* stream_state, int media_type, unsigned* pid, unsigned* sid);
void mpegts_encoder_finalize_streams(mpegts_encoder_init_streams_state_t* stream_state);

vod_status_t mpegts_encoder_flush(mpegts_encoder_state_t* state);

void mpegts_encoder_simulated_start_segment(mpegts_encoder_state_t* state);
uint32_t mpegts_encoder_simulated_get_offset(mpegts_encoder_state_t* state);

#endif // __MPEGTS_ENCODER_FILTER_H__
