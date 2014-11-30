#ifndef __WRITE_BUFFER_QUEUE_H__
#define __WRITE_BUFFER_QUEUE_H__

// includes
#include "list_entry.h"
#include "common.h"

// typedefs
typedef struct {
	list_entry_t link;
	u_char* start_pos;
	u_char* cur_pos;
	u_char* end_pos;
} buffer_header_t;

typedef struct {
	// input params
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;

	list_entry_t buffers;
	buffer_header_t* cur_write_buffer;
} write_buffer_queue_t;

// functions
void write_buffer_queue_init(write_buffer_queue_t* queue, request_context_t* request_context);
u_char* write_buffer_queue_get_buffer(write_buffer_queue_t* queue, uint32_t size);
vod_status_t write_buffer_queue_send(write_buffer_queue_t* queue, u_char* ptr);
vod_status_t write_buffer_queue_flush(write_buffer_queue_t* queue);

#endif // __WRITE_BUFFER_QUEUE_H__
