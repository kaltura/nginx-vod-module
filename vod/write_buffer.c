#include "write_buffer.h"

// constants
#define WRITE_BUFFER_SIZE (65536)

void
write_buffer_init(
	write_buffer_state_t* state,
	request_context_t* request_context,
	write_callback_t write_callback,
	void* write_context)
{
	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	state->start_pos = state->end_pos = state->cur_pos = NULL;
}

vod_status_t
write_buffer_flush(write_buffer_state_t* state, bool_t reallocate)
{
	vod_status_t rc;
	bool_t reuse_buffer;

	if (state->cur_pos > state->start_pos)
	{
		rc = state->write_callback(state->write_context, state->start_pos, state->cur_pos - state->start_pos, &reuse_buffer);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"write_buffer_flush: write_callback failed %i", rc);
			return rc;
		}

		if (reuse_buffer)
		{
			state->cur_pos = state->start_pos;
			return VOD_OK;
		}
	}

	if (reallocate)
	{
		state->start_pos = vod_alloc(state->request_context->pool, WRITE_BUFFER_SIZE);
		if (state->start_pos == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"write_buffer_flush: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
		state->end_pos = state->start_pos + WRITE_BUFFER_SIZE;
		state->cur_pos = state->start_pos;
	}
	else
	{
		state->start_pos = state->end_pos = state->cur_pos = NULL;
	}

	return VOD_OK;
}

vod_status_t
write_buffer_get_bytes(
	write_buffer_state_t* state,
	size_t size,
	u_char** buffer)
{
	vod_status_t rc;

	if (size > WRITE_BUFFER_SIZE)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"write_buffer_get_bytes: invalid size request %uz", size);
		return VOD_UNEXPECTED;
	}

	if (state->cur_pos + size > state->end_pos)
	{
		rc = write_buffer_flush(state, TRUE);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	*buffer = state->cur_pos;
	state->cur_pos += size;
	return VOD_OK;
}

vod_status_t
write_buffer_write(
	write_buffer_state_t* state,
	const u_char* buffer,
	size_t size)
{
	vod_status_t rc;
	size_t cur_copy_size;

	for (;;)
	{
		cur_copy_size = state->end_pos - state->cur_pos;
		if (cur_copy_size > size)
		{
			cur_copy_size = size;
		}
		state->cur_pos = vod_copy(state->cur_pos, buffer, cur_copy_size);
		size -= cur_copy_size;
		if (size <= 0)
		{
			break;
		}
		buffer += cur_copy_size;

		rc = write_buffer_flush(state, TRUE);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}
