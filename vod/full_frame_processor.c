#include "full_frame_processor.h"

vod_status_t 
full_frame_processor_init(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	read_cache_state_t* read_cache_state,
	frame_callback_t frame_callback,
	void* frame_context,
	full_frame_processor_t** result)
{
	full_frame_processor_t* state;
	input_frame_t* cur_frame;
	uint32_t max_frame_size = 0;

	state = vod_alloc(request_context->pool, sizeof(full_frame_processor_t));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"full_frame_processor_init: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->frame_callback = frame_callback;
	state->frame_context = frame_context;

	state->read_cache_state = read_cache_state;
	state->cur_frame = stream_metadata->frames;
	state->last_frame = stream_metadata->frames + stream_metadata->frame_count;
	state->cur_frame_offset = stream_metadata->frame_offsets;
	state->frames_file_index = stream_metadata->frames_file_index;
	state->cur_frame_pos = 0;
	state->first_time = TRUE;

	// find the max frame size
	for (cur_frame = state->cur_frame; cur_frame < state->last_frame; cur_frame++)
	{
		if (cur_frame->size > max_frame_size)
		{
			max_frame_size = cur_frame->size;
		}
	}

	state->frame_buffer = vod_alloc(request_context->pool, max_frame_size);
	if (state->frame_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"full_frame_processor_init: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	*result = state;
	return VOD_OK;
}

vod_status_t
full_frame_processor_process(full_frame_processor_t* state)
{
	u_char* read_buffer;
	uint32_t read_size;
	uint32_t cur_copy_size;
	uint64_t offset;
	bool_t processed_data = FALSE;
	vod_status_t rc;

	for (;;)
	{
		// check if we're done
		if (state->cur_frame >= state->last_frame)
		{
			rc = state->frame_callback(state->frame_context, NULL, NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}			
			return VOD_OK;
		}

		// read some data from the frame
		offset = *state->cur_frame_offset + state->cur_frame_pos;
		if (!read_cache_get_from_cache(
			state->read_cache_state, 
			state->cur_frame->size - state->cur_frame_pos, 
			0, 
			state->frames_file_index, 
			offset, 
			&read_buffer, 
			&read_size))
		{
			if (!processed_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"full_frame_processor_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}
		
		processed_data = TRUE;

		if (state->cur_frame_pos == 0 && read_size >= state->cur_frame->size)
		{
			// have the whole frame in one piece
			rc = state->frame_callback(state->frame_context, state->cur_frame, read_buffer);
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			// move to the next frame
			state->cur_frame++;
			state->cur_frame_offset++;
			continue;
		}
		
		// copy as much as possible from the frame
		cur_copy_size = vod_min(state->cur_frame->size - state->cur_frame_pos, read_size);
		vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, cur_copy_size);
		state->cur_frame_pos += cur_copy_size;

		// move to the next frame if done
		if (state->cur_frame_pos >= state->cur_frame->size)
		{
			rc = state->frame_callback(state->frame_context, state->cur_frame, state->frame_buffer);
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			state->cur_frame++;
			state->cur_frame_offset++;
			state->cur_frame_pos = 0;
		}
	}
}
