#ifndef __BUFFER_FILTER_H__
#define __BUFFER_FILTER_H__

// include
#include "media_filter.h"

// typedefs
typedef struct {
	// input data
	request_context_t* request_context;
	const media_filter_t* next_filter;
	void* next_filter_context;
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
	int32_t margin_size;
} buffer_filter_t;

// globals
extern const media_filter_t buffer_filter;

// functions
vod_status_t buffer_filter_init(
	buffer_filter_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	uint32_t size);
vod_status_t buffer_filter_force_flush(buffer_filter_t* state);
bool_t buffer_filter_get_dts(buffer_filter_t* state, uint64_t* dts);

void buffer_filter_simulated_force_flush(buffer_filter_t* state);

#endif // __BUFFER_FILTER_H__
