#include "frames_source_memory.h"
#include "../media_format.h"

// typedefs
typedef struct {
	u_char* buffer;
	size_t size;
} frames_source_memory_state_t;

vod_status_t
frames_source_memory_init(
	request_context_t* request_context,
	void** result)
{
	frames_source_memory_state_t* state;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frames_source_memory_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	*result = state;

	return VOD_OK;
}

static void
frames_source_memory_set_cache_slot_id(void* ctx, int cache_slot_id)
{
}

static vod_status_t
frames_source_memory_start_frame(void* ctx, input_frame_t* frame, read_cache_hint_t* cache_hint)
{
	frames_source_memory_state_t* state = ctx;

	state->buffer = (u_char*)(uintptr_t)frame->offset;
	state->size = frame->size;

	return VOD_OK;
}

static vod_status_t
frames_source_memory_read(void* ctx, u_char** buffer, uint32_t* size, bool_t* frame_done)
{
	frames_source_memory_state_t* state = ctx;

	*buffer = state->buffer;
	*size = state->size;
	*frame_done = TRUE;

	return VOD_OK;
}

static void
frames_source_memory_disable_buffer_reuse(void* ctx)
{
}

static vod_status_t
frames_source_memory_skip_frames(void* ctx, uint32_t skip_count)
{
	return VOD_OK;
}

// globals
frames_source_t frames_source_memory = {
	frames_source_memory_set_cache_slot_id,
	frames_source_memory_start_frame,
	frames_source_memory_read,
	frames_source_memory_disable_buffer_reuse,
	frames_source_memory_skip_frames,
};
