#include "mp4_builder.h"

u_char*
mp4_builder_write_mfhd_atom(u_char* p, uint32_t segment_index)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(mfhd_atom_t);

	write_atom_header(p, atom_size, 'm', 'f', 'h', 'd');
	write_dword(p, 0);
	write_dword(p, segment_index);
	return p;
}

size_t
mp4_builder_get_trun_atom_size(uint32_t media_type, uint32_t frame_count)
{
	switch (media_type)
	{
	case MEDIA_TYPE_VIDEO:
		return ATOM_HEADER_SIZE + sizeof(trun_atom_t) + frame_count * 4 * sizeof(uint32_t);

	case MEDIA_TYPE_AUDIO:
		return ATOM_HEADER_SIZE + sizeof(trun_atom_t) + frame_count * 2 * sizeof(uint32_t);
	}
	return 0;
}

static u_char* 
mp4_builder_write_video_trun_atom(u_char* p, input_frame_t* frames, uint32_t frame_count, uint32_t first_frame_offset)
{
	input_frame_t* cur_frame;
	input_frame_t* last_frame = frames + frame_count;
	size_t atom_size;

	atom_size = ATOM_HEADER_SIZE + sizeof(trun_atom_t) + frame_count * 4 * sizeof(uint32_t);

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_dword(p, 0xF01);								// flags = data offset, duration, size, key, delay
	write_dword(p, frame_count);
	write_dword(p, first_frame_offset);	// first frame offset relative to moof start offset

	for (cur_frame = frames; cur_frame < last_frame; cur_frame++)
	{
		write_dword(p, cur_frame->duration);
		write_dword(p, cur_frame->size);
		if (cur_frame->key_frame)
		{
			write_dword(p, 0x00000000);
		}
		else
		{
			write_dword(p, 0x00010000);
		}
		write_dword(p, cur_frame->pts_delay);
	}
	return p;
}

static u_char* 
mp4_builder_write_audio_trun_atom(u_char* p, input_frame_t* frames, uint32_t frame_count, uint32_t first_frame_offset)
{
	input_frame_t* cur_frame;
	input_frame_t* last_frame = frames + frame_count;
	size_t atom_size;	

	atom_size = ATOM_HEADER_SIZE + sizeof(trun_atom_t) + frame_count * 2 * sizeof(uint32_t);

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_dword(p, 0x301);								// flags = data offset, duration, size
	write_dword(p, frame_count);
	write_dword(p, first_frame_offset);	// first frame offset relative to moof start offset

	for (cur_frame = frames; cur_frame < last_frame; cur_frame++)
	{
		write_dword(p, cur_frame->duration);
		write_dword(p, cur_frame->size);
	}
	return p;
}

u_char*
mp4_builder_write_trun_atom(
	u_char* p, 
	uint32_t media_type, 
	input_frame_t* frames, 
	uint32_t frame_count, 
	uint32_t first_frame_offset)
{
	switch (media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = mp4_builder_write_video_trun_atom(p, frames, frame_count, first_frame_offset);
		break;

	case MEDIA_TYPE_AUDIO:
		p = mp4_builder_write_audio_trun_atom(p, frames, frame_count, first_frame_offset);
		break;
	}
	return p;
}

vod_status_t 
mp4_builder_frame_writer_init(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	read_cache_state_t* read_cache_state,
	write_callback_t write_callback,
	void* write_context, 
	fragment_writer_state_t** result)
{
	fragment_writer_state_t* state;

	state = vod_alloc(request_context->pool, sizeof(fragment_writer_state_t));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_builder_frame_writer_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	state->frames_file_index = stream_metadata->frames_file_index;

	state->read_cache_state = read_cache_state;
	state->cur_frame = stream_metadata->frames;
	state->last_frame = stream_metadata->frames + stream_metadata->frame_count;
	state->cur_frame_offset = stream_metadata->frame_offsets;
	state->cur_frame_pos = 0;
	state->first_time = TRUE;

	*result = state;
	return VOD_OK;
}

vod_status_t
mp4_builder_frame_writer_process(fragment_writer_state_t* state)
{
	u_char* read_buffer;
	uint32_t read_size;
	u_char* write_buffer = NULL;
	uint32_t write_buffer_size = 0;
	uint32_t cur_write_size;
	uint64_t offset;
	vod_status_t rc;
	bool_t reuse_buffer = TRUE;
	bool_t cur_reuse_buffer;

	for (;;)
	{
		if (state->cur_frame >= state->last_frame)
		{
			if (write_buffer != NULL)
			{
				// flush the write buffer
				rc = state->write_callback(state->write_context, write_buffer, write_buffer_size, &cur_reuse_buffer);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			return VOD_OK;
		}

		// read some data from the frame
		offset = *state->cur_frame_offset + state->cur_frame_pos;
		if (!read_cache_get_from_cache(state->read_cache_state, state->cur_frame->size - state->cur_frame_pos, 0, state->frames_file_index, offset, &read_buffer, &read_size))
		{
			if (write_buffer != NULL)
			{
				// flush the write buffer
				rc = state->write_callback(state->write_context, write_buffer, write_buffer_size, &cur_reuse_buffer);
				if (rc != VOD_OK)
				{
					return rc;
				}

				reuse_buffer = reuse_buffer && cur_reuse_buffer;
			}
			else if (!state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_builder_frame_writer_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			if (!reuse_buffer)
			{
				read_cache_disable_buffer_reuse(state->read_cache_state);
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}

		// update the frame position
		cur_write_size = vod_min(state->cur_frame->size - state->cur_frame_pos, read_size);
		state->cur_frame_pos += cur_write_size;

		// move to the next frame if done
		if (state->cur_frame_pos >= state->cur_frame->size)
		{
			state->cur_frame++;
			state->cur_frame_offset++;
			state->cur_frame_pos = 0;
		}

		if (write_buffer != NULL)
		{
			// if the buffers are contiguous, just increment the size
			if (write_buffer + write_buffer_size == read_buffer)
			{
				write_buffer_size += cur_write_size;
				continue;
			}

			// buffers not continguous, flush the write buffer
			rc = state->write_callback(state->write_context, write_buffer, write_buffer_size, &cur_reuse_buffer);
			if (rc != VOD_OK)
			{
				return rc;
			}

			reuse_buffer = reuse_buffer && cur_reuse_buffer;
		}

		// reset the write buffer
		write_buffer = read_buffer;
		write_buffer_size = cur_write_size;
	}
}
