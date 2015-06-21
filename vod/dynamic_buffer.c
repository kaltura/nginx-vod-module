#include "dynamic_buffer.h"

vod_status_t
vod_dynamic_buf_init(vod_dynamic_buf_t* buffer, request_context_t* request_context, size_t initial_size)
{
	buffer->request_context = request_context;
	buffer->start = vod_alloc(request_context->pool, initial_size);
	if (buffer->start == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"vod_dynamic_buf_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	buffer->end = buffer->start + initial_size;
	buffer->pos = buffer->start;
	return VOD_OK;
}

vod_status_t
vod_dynamic_buf_reserve(vod_dynamic_buf_t* buffer, size_t size)
{
	u_char* new_buffer;
	size_t used_buffer_size;
	size_t new_size;

	if (buffer->pos + size <= buffer->end)
	{
		return VOD_OK;
	}

	new_size = 2 * (buffer->end - buffer->start);
	new_size = vod_max(new_size, size);

	new_buffer = vod_alloc(buffer->request_context->pool, new_size);
	if (new_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, buffer->request_context->log, 0,
			"vod_dynamic_buf_reserve: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	used_buffer_size = buffer->pos - buffer->start;
	vod_memcpy(new_buffer, buffer->start, used_buffer_size);
	buffer->start = new_buffer;
	buffer->end = new_buffer + new_size;
	buffer->pos = new_buffer + used_buffer_size;

	return VOD_OK;
}

