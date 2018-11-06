#include "frames_source_cache.h"
#include "../media_format.h"

vod_status_t
frames_source_cache_init(
	request_context_t* request_context,
	read_cache_state_t* read_cache_state,
	void* source,
	int cache_slot_id,
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
	state->req.source = source;
	state->req.cache_slot_id = cache_slot_id;

	*result = state;

	return VOD_OK;
}

static void
frames_source_cache_set_cache_slot_id(void* ctx, int cache_slot_id)
{
	frames_source_cache_state_t* state = ctx;

	state->req.cache_slot_id = cache_slot_id;
}

static vod_status_t
frames_source_cache_start_frame(void* ctx, input_frame_t* frame, read_cache_hint_t* cache_hint)
{
	frames_source_cache_state_t* state = ctx;

	state->req.cur_offset = frame->offset;
	state->req.end_offset = frame->offset + frame->size;
	if (cache_hint != NULL)
	{
		state->req.hint = *cache_hint;
	}
	else
	{
		state->req.hint.min_offset = ULLONG_MAX;
	}

	return VOD_OK;
}

static vod_status_t
frames_source_cache_read(void* ctx, u_char** buffer, uint32_t* size, bool_t* frame_done)
{
	frames_source_cache_state_t* state = ctx;
	uint64_t cur_end_offset;

	if (!read_cache_get_from_cache(
		state->read_cache_state,
		&state->req,
		buffer,
		size))
	{
		return VOD_AGAIN;
	}

	cur_end_offset = state->req.cur_offset + *size;
	if (cur_end_offset >= state->req.end_offset)
	{
		*size = state->req.end_offset - state->req.cur_offset;
		*frame_done = TRUE;
		state->req.cur_offset = state->req.end_offset;
	}
	else
	{
		*frame_done = FALSE;
		state->req.cur_offset = cur_end_offset;
	}
	return VOD_OK;
}

static void
frames_source_cache_disable_buffer_reuse(void* ctx)
{
	frames_source_cache_state_t* state = ctx;

	read_cache_disable_buffer_reuse(state->read_cache_state);
}

static vod_status_t
frames_source_cache_skip_frames(void* ctx, uint32_t skip_count)
{
	return VOD_OK;
}

// globals
frames_source_t frames_source_cache = {
	frames_source_cache_set_cache_slot_id,
	frames_source_cache_start_frame,
	frames_source_cache_read,
	frames_source_cache_disable_buffer_reuse,
	frames_source_cache_skip_frames,
};
