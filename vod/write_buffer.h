#ifndef __WRITE_BUFFER_H__
#define __WRITE_BUFFER_H__

// includes
#include "common.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;
	bool_t reuse_buffers;

	u_char* start_pos;
	u_char* cur_pos;
	u_char* end_pos;
} write_buffer_state_t;

// functions
void write_buffer_init(
	write_buffer_state_t* state,
	request_context_t* request_context,
	write_callback_t write_callback,
	void* write_context,
	bool_t reuse_buffers);

vod_status_t write_buffer_flush(write_buffer_state_t* state, bool_t reallocate);

vod_status_t write_buffer_get_bytes(
	write_buffer_state_t* state,
	size_t min_size,
	size_t* size,
	u_char** buffer);

vod_status_t write_buffer_write(
	write_buffer_state_t* state,
	const u_char* buffer,
	size_t size);

#endif // __WRITE_BUFFER_H__
