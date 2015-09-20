#ifndef __BUFFER_FILTER_H__
#define __BUFFER_FILTER_H__

// include
#include "media_filter.h"

#define BUFFERED_FRAMES_QUEUE_SIZE (28)		// 28 = ceil(188/7) + 1, 188 = ts packet size, 7 = min audio frame size (adts header)

// typedefs
typedef struct {
	output_frame_t frame;
	u_char* end_pos;
} buffered_frame_info_t;

typedef struct {
	buffered_frame_info_t data[BUFFERED_FRAMES_QUEUE_SIZE];
	uint32_t write_pos;
	uint32_t read_pos;
	bool_t is_full;
} buffered_frames_queue_t;

typedef struct {
	// input data
	request_context_t* request_context;
	const media_filter_t* next_filter;
	void* next_filter_context;
	bool_t align_frames;
	uint32_t size;

	// fixed
	u_char* start_pos;
	u_char* end_pos;

	// state
	int cur_state;
	output_frame_t cur_frame;
	output_frame_t last_frame;
	u_char* cur_pos;
	u_char* last_flush_pos;

	buffered_frames_queue_t buffered_frames;

	// simulation mode
	uint32_t used_size;
	uint32_t last_flush_size;
} buffer_filter_t;

// globals
extern const media_filter_t buffer_filter;

// functions
vod_status_t buffer_filter_init(
	buffer_filter_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	bool_t align_frames,
	uint32_t size);
vod_status_t buffer_filter_force_flush(buffer_filter_t* state, bool_t last_stream_frame);
bool_t buffer_filter_get_dts(buffer_filter_t* state, uint64_t* dts);

void buffer_filter_simulated_force_flush(buffer_filter_t* state, bool_t last_stream_frame);

#endif // __BUFFER_FILTER_H__
