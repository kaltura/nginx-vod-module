#include "read_cache.h"

#define MIN_BUFFER_COUNT (2)

void 
read_cache_init(read_cache_state_t* state, request_context_t* request_context, size_t buffer_size, size_t alignment)
{
	state->request_context = request_context;
	state->buffer_size = buffer_size;
	state->alignment = alignment;
	state->buffer_count = 0;
	state->reuse_buffers = TRUE;
}

vod_status_t
read_cache_allocate_buffer_slots(read_cache_state_t* state, size_t buffer_count)
{
	size_t alloc_size;

	if (buffer_count < MIN_BUFFER_COUNT)
	{
		buffer_count = MIN_BUFFER_COUNT;
	}

	if (state->buffer_count >= buffer_count)
	{
		return VOD_OK;
	}

	alloc_size = sizeof(state->buffers[0]) * buffer_count;

	state->buffers = vod_alloc(state->request_context->pool, alloc_size);
	if (state->buffers == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"read_cache_allocate_buffer_slots: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->buffers_end = state->buffers + buffer_count;
	state->buffer_count = buffer_count;

	vod_memzero(state->buffers, alloc_size);

	return VOD_OK;
}

bool_t 
read_cache_get_from_cache(
	read_cache_state_t* state, 
	int cache_slot_id, 
	void* source, 
	uint64_t offset, 
	u_char** buffer, 
	uint32_t* size)
{
	cache_buffer_t* cur_buffer;

	// check whether we already have the requested offset
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer->source == source && 
			offset >= cur_buffer->start_offset && offset < cur_buffer->end_offset)
		{
			*buffer = cur_buffer->buffer_pos + (offset - cur_buffer->start_offset);
			*size = cur_buffer->end_offset - offset;
			return TRUE;
		}
	}

	// don't have the offset in cache
	cache_slot_id %= state->buffer_count;
	state->target_buffer = &state->buffers[cache_slot_id];
	state->target_buffer->source = source;
	state->target_buffer->start_offset = (offset & ~(state->alignment - 1));

	return FALSE;
}

void
read_cache_disable_buffer_reuse(read_cache_state_t* state)
{
	state->reuse_buffers = FALSE;
}

void 
read_cache_get_read_buffer(
	read_cache_state_t* state, 
	void** source, 
	uint64_t* out_offset, 
	u_char** buffer, 
	uint32_t* size)
{
	cache_buffer_t* target_buffer = state->target_buffer;
	cache_buffer_t* cur_buffer;
	uint32_t read_size;
	
	// make sure we don't read anything we already have in cache
	read_size = state->buffer_size;
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer != target_buffer && 
			cur_buffer->source == target_buffer->source &&
			cur_buffer->start_offset > target_buffer->start_offset)
		{
			read_size = vod_min(read_size, cur_buffer->start_offset - target_buffer->start_offset);
		}
	}
	
	// return the target buffer pointer and size
	*source = target_buffer->source;
	*out_offset = target_buffer->start_offset;
	*buffer = state->reuse_buffers ? target_buffer->buffer_start : NULL;
	*size = read_size;
}

void 
read_cache_read_completed(read_cache_state_t* state, vod_buf_t* buf)
{
	cache_buffer_t* target_buffer = state->target_buffer;

	// update the buffer size
	target_buffer->buffer_start = buf->start;
	target_buffer->buffer_pos = buf->pos;
	target_buffer->buffer_size = buf->last - buf->pos;
	target_buffer->end_offset = target_buffer->start_offset + target_buffer->buffer_size;

	// no longer have an active request
	state->target_buffer = NULL;
}
