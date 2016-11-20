#ifndef __WRITE_BUFFER_QUEUE_H__
#define __WRITE_BUFFER_QUEUE_H__

// includes
#include "common.h"

// typedefs
typedef struct {
	vod_queue_t link;
	u_char* start_pos;
	u_char* cur_pos;
	u_char* end_pos;
	off_t end_offset;
} buffer_header_t;

typedef struct {
	// input params
	request_context_t* request_context;
	buffer_pool_t* output_buffer_pool;
	write_callback_t write_callback;
	void* write_context;
	bool_t reuse_buffers;

	vod_queue_t buffers;
	buffer_header_t* cur_write_buffer;

	void* last_writer_context;
	off_t cur_offset;
} write_buffer_queue_t;

// functions
void write_buffer_queue_init(
	write_buffer_queue_t* queue, 
	request_context_t* request_context, 
	write_callback_t write_callback,
	void* write_context,
	bool_t reuse_buffers);
u_char* write_buffer_queue_get_buffer(write_buffer_queue_t* queue, uint32_t size, void* writer_context);
vod_status_t write_buffer_queue_send(write_buffer_queue_t* queue, off_t max_offset);
vod_status_t write_buffer_queue_flush(write_buffer_queue_t* queue);

#endif // __WRITE_BUFFER_QUEUE_H__
