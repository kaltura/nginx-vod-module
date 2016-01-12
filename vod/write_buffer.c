#include "write_buffer.h"
#include "buffer_pool.h"

// constants
#define WRITE_BUFFER_SIZE (65536)

void
write_buffer_init(
	write_buffer_state_t* state,
	request_context_t* request_context,
	write_callback_t write_callback,
	void* write_context, 
	bool_t reuse_buffers)
{
	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	state->reuse_buffers = reuse_buffers;
	state->start_pos = state->end_pos = state->cur_pos = NULL;
}

vod_status_t
write_buffer_flush(write_buffer_state_t* state, bool_t reallocate)
{
	vod_status_t rc;
	size_t buffer_size;

	if (state->cur_pos > state->start_pos)
	{
		rc = state->write_callback(state->write_context, state->start_pos, state->cur_pos - state->start_pos);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"write_buffer_flush: write_callback failed %i", rc);
			return rc;
		}

		if (state->reuse_buffers)
		{
			state->cur_pos = state->start_pos;
			return VOD_OK;
		}
	}

	if (reallocate)
	{
		buffer_size = WRITE_BUFFER_SIZE;
		state->start_pos = buffer_pool_alloc(
			state->request_context, 
			state->request_context->output_buffer_pool, 
			&buffer_size);
		if (state->start_pos == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"write_buffer_flush: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
		state->end_pos = state->start_pos + buffer_size;
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
	size_t min_size,
	size_t* size,
	u_char** buffer)
{
	vod_status_t rc;

	if (state->cur_pos + min_size > state->end_pos)
	{
		rc = write_buffer_flush(state, TRUE);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	if (min_size > (size_t)(state->end_pos - state->start_pos))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"write_buffer_get_bytes: invalid size request %uz", min_size);
		return VOD_UNEXPECTED;
	}

	*buffer = state->cur_pos;
	if (size != NULL)
	{
		// Note: in this mode, the caller is responsible for incrementing cur_pos
		*size = state->end_pos - state->cur_pos;
	}
	else
	{
		state->cur_pos += min_size;
	}
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
