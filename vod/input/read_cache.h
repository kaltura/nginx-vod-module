#ifndef __READ_CACHE_H__
#define __READ_CACHE_H__

// includes
#include "../common.h"

// typedefs
struct media_clip_source_s;

typedef struct {
	u_char* buffer_start;
	u_char* buffer_pos;
	uint32_t buffer_size;		// size of data read
	void* source;				// opaque context that indicates from where the buffer should be read
	uint64_t start_offset;
	uint64_t end_offset;
} cache_buffer_t;

typedef struct {
	request_context_t* request_context;
	cache_buffer_t* buffers;
	cache_buffer_t* buffers_end;
	cache_buffer_t* target_buffer;
	size_t buffer_count;
	size_t buffer_size;
	bool_t reuse_buffers;
} read_cache_state_t;

typedef struct {
	uint64_t min_offset;
	int min_offset_slot_id;
} read_cache_hint_t;

typedef struct {
	int cache_slot_id;
	struct media_clip_source_s* source;
	uint64_t cur_offset;
	uint64_t end_offset;
	read_cache_hint_t hint;
} read_cache_request_t;

typedef struct {
	struct media_clip_source_s* source;
	uint64_t offset;
	u_char* buffer;
	uint32_t size;
} read_cache_get_read_buffer_t;

// functions
void read_cache_init(
	read_cache_state_t* state, 
	request_context_t* request_context, 
	size_t buffer_size);
	
vod_status_t read_cache_allocate_buffer_slots(
	read_cache_state_t* state,
	size_t buffer_count);

bool_t read_cache_get_from_cache(
	read_cache_state_t* state, 
	read_cache_request_t* request,
	u_char** buffer,
	uint32_t* size);

void read_cache_disable_buffer_reuse(
	read_cache_state_t* state);

void read_cache_get_read_buffer(
	read_cache_state_t* state, 
	read_cache_get_read_buffer_t* result);
	
void read_cache_read_completed(read_cache_state_t* state, vod_buf_t* buf);

#endif // __READ_CACHE_H__
