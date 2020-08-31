#include "read_cache.h"
#include "../media_clip.h"

#define MIN_BUFFER_COUNT (2)

void 
read_cache_init(read_cache_state_t* state, request_context_t* request_context, size_t buffer_size)
{
	state->request_context = request_context;
	state->buffer_size = buffer_size;
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
	read_cache_request_t* request,
	u_char** buffer,
	uint32_t* size)
{
	media_clip_source_t* source = request->source;
	read_cache_hint_t* hint;
	cache_buffer_t* target_buffer;
	cache_buffer_t* cur_buffer;
	uint32_t read_size;
	uint64_t aligned_last_offset;
	uint64_t offset = request->cur_offset;
	size_t alignment;
	int cache_slot_id;

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
	alignment = source->alignment - 1;
	cache_slot_id = request->cache_slot_id;

	// start reading from the min offset, if that would contain the whole frame
	// Note: this condition is intended to optimize the case in which the frame order 
	//		in the output segment is <video1><audio1> while on disk it's <audio1><video1>. 
	//		in this case it would be better to start reading from the beginning, even 
	//		though the first frame that is requested is the second one
	hint = &request->hint;
	if (hint->min_offset < offset && 
		hint->min_offset + state->buffer_size / 4 > offset &&
		request->end_offset < (hint->min_offset & ~alignment) + state->buffer_size)
	{
		offset = hint->min_offset;
		cache_slot_id = hint->min_offset_slot_id;
	}
	offset &= ~alignment;

	// calculate the read size
	read_size = state->buffer_size;
	target_buffer = &state->buffers[cache_slot_id % state->buffer_count];

	// don't read anything that is already in the cache
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer == target_buffer ||
			cur_buffer->source != source)
		{
			continue;
		}

		if (cur_buffer->start_offset > offset)
		{
			read_size = vod_min(read_size, cur_buffer->start_offset - offset);
		}
		else if (cur_buffer->end_offset > offset)
		{
			offset = cur_buffer->end_offset & ~alignment;
		}
	}

	// don't read past the max required offset
	if (offset + read_size > source->last_offset)
	{
		aligned_last_offset = (source->last_offset + alignment) & ~alignment;
		if (aligned_last_offset > offset)
		{
			read_size = aligned_last_offset - offset;
		}
	}

	target_buffer->source = source;
	target_buffer->start_offset = offset;
	target_buffer->buffer_size = read_size;
	state->target_buffer = target_buffer;

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
	read_cache_get_read_buffer_t* result)
{
	cache_buffer_t* target_buffer = state->target_buffer;
		
	// return the target buffer pointer and size
	result->source = target_buffer->source;
	result->offset = target_buffer->start_offset;
	result->buffer = state->reuse_buffers ? target_buffer->buffer_start : NULL;
	result->size = target_buffer->buffer_size;
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
