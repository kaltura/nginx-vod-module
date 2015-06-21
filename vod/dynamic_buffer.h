#ifndef __DYNAMIC_BUFFER_H__
#define __DYNAMIC_BUFFER_H__

// includes
#include "common.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	u_char* start;
	u_char* pos;
	u_char* end;
} vod_dynamic_buf_t;

// functions
vod_status_t vod_dynamic_buf_init(vod_dynamic_buf_t* buffer, request_context_t* request_context, size_t initial_size);

vod_status_t vod_dynamic_buf_reserve(vod_dynamic_buf_t* buffer, size_t size);

#endif // __DYNAMIC_BUFFER_H__
