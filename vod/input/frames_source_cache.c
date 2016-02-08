#include "frames_source_cache.h"
#include "../media_format.h"

// typedefs
typedef struct {
	read_cache_state_t* read_cache_state;
	void* source;
	int cache_slot_id;
	uint64_t cur_offset;
	uint64_t end_offset;
} frames_source_cache_state_t;

vod_status_t
frames_source_cache_init(
	request_context_t* request_context,
	read_cache_state_t* read_cache_state,
	void* source,
	void** result)
{
	frames_source_cache_state_t* state;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frames_source_cache_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->read_cache_state = read_cache_state;
	state->source = source;

	*result = state;

	return VOD_OK;
}

static void
frames_source_cache_set_cache_slot_id(void* ctx, int cache_slot_id)
{
	frames_source_cache_state_t* state = ctx;

	state->cache_slot_id = cache_slot_id;
}

static vod_status_t
frames_source_cache_start_frame(void* ctx, input_frame_t* frame)
{
	frames_source_cache_state_t* state = ctx;

	state->cur_offset = frame->offset;
	state->end_offset = frame->offset + frame->size;

	return VOD_OK;
}

static vod_status_t
frames_source_cache_read(void* ctx, u_char** buffer, uint32_t* size, bool_t* frame_done)
{
	frames_source_cache_state_t* state = ctx;
	uint64_t cur_end_offset;

	if (!read_cache_get_from_cache(
		state->read_cache_state,
		state->cache_slot_id,
		state->source,
		state->cur_offset,
		buffer,
		size))
	{
		return VOD_AGAIN;
	}

	cur_end_offset = state->cur_offset + *size;
	if (cur_end_offset >= state->end_offset)
	{
		*size = state->end_offset - state->cur_offset;
		*frame_done = TRUE;
		state->cur_offset = state->end_offset;
	}
	else
	{
		*frame_done = FALSE;
		state->cur_offset = cur_end_offset;
	}
	return VOD_OK;
}

static void
frames_source_cache_disable_buffer_reuse(void* ctx)
{
	frames_source_cache_state_t* state = ctx;

	read_cache_disable_buffer_reuse(state->read_cache_state);
}

// globals
frames_source_t frames_source_cache = {
	frames_source_cache_set_cache_slot_id,
	frames_source_cache_start_frame,
	frames_source_cache_read,
	frames_source_cache_disable_buffer_reuse,
};
