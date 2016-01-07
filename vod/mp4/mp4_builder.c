#include "mp4_builder.h"

u_char*
mp4_builder_write_mfhd_atom(u_char* p, uint32_t segment_index)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(mfhd_atom_t);

	write_atom_header(p, atom_size, 'm', 'f', 'h', 'd');
	write_be32(p, 0);
	write_be32(p, segment_index);
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
mp4_builder_write_video_trun_atom(u_char* p, media_sequence_t* sequence, uint32_t first_frame_offset)
{
	media_clip_filtered_t* cur_clip;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	size_t atom_size;

	atom_size = ATOM_HEADER_SIZE + sizeof(trun_atom_t) + sequence->total_frame_count * 4 * sizeof(uint32_t);

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, 0xF01);								// flags = data offset, duration, size, key, delay
	write_be32(p, sequence->total_frame_count);
	write_be32(p, first_frame_offset);	// first frame offset relative to moof start offset

	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		cur_frame = cur_clip->first_track->first_frame;
		last_frame = cur_clip->first_track->last_frame;
		for (; cur_frame < last_frame; cur_frame++)
		{
			write_be32(p, cur_frame->duration);
			write_be32(p, cur_frame->size);
			if (cur_frame->key_frame)
			{
				write_be32(p, 0x00000000);
			}
			else
			{
				write_be32(p, 0x00010000);
			}
			write_be32(p, cur_frame->pts_delay);
		}
	}
	return p;
}

static u_char* 
mp4_builder_write_audio_trun_atom(u_char* p, media_sequence_t* sequence, uint32_t first_frame_offset)
{
	media_clip_filtered_t* cur_clip;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	size_t atom_size;

	atom_size = ATOM_HEADER_SIZE + sizeof(trun_atom_t) + sequence->total_frame_count * 2 * sizeof(uint32_t);

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, 0x301);								// flags = data offset, duration, size
	write_be32(p, sequence->total_frame_count);
	write_be32(p, first_frame_offset);	// first frame offset relative to moof start offset

	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		cur_frame = cur_clip->first_track->first_frame;
		last_frame = cur_clip->first_track->last_frame;
		for (; cur_frame < last_frame; cur_frame++)
		{
			write_be32(p, cur_frame->duration);
			write_be32(p, cur_frame->size);
		}
	}
	return p;
}

u_char*
mp4_builder_write_trun_atom(
	u_char* p, 
	media_sequence_t* sequence,
	uint32_t first_frame_offset)
{
	switch (sequence->media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = mp4_builder_write_video_trun_atom(p, sequence, first_frame_offset);
		break;

	case MEDIA_TYPE_AUDIO:
		p = mp4_builder_write_audio_trun_atom(p, sequence, first_frame_offset);
		break;
	}
	return p;
}

static void
mp4_builder_init_track(fragment_writer_state_t* state, media_track_t* track)
{
	state->first_time = TRUE;
	state->frames_source = track->frames_source;
	state->frames_source_context = track->frames_source_context;
	state->cur_frame = track->first_frame;
	state->last_frame = track->last_frame;
	state->cur_frame_offset = track->frame_offsets;

	if (!state->reuse_buffers)
	{
		state->frames_source->disable_buffer_reuse(state->frames_source_context);
	}
}

vod_status_t 
mp4_builder_frame_writer_init(
	request_context_t* request_context,
	media_sequence_t* sequence,
	write_callback_t write_callback,
	void* write_context, 
	bool_t reuse_buffers,
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
	state->reuse_buffers = reuse_buffers;
	state->frame_started = FALSE;
	state->sequence = sequence;
	state->cur_clip = sequence->filtered_clips;

	mp4_builder_init_track(state, state->cur_clip->first_track);

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
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		while (state->cur_frame >= state->last_frame)
		{
			if (write_buffer != NULL)
			{
				// flush the write buffer
				rc = state->write_callback(state->write_context, write_buffer, write_buffer_size);
				if (rc != VOD_OK)
				{
					return rc;
				}

				write_buffer = NULL;
			}

			state->cur_clip++;
			if (state->cur_clip >= state->sequence->filtered_clips_end)
			{
				return VOD_OK;
			}

			mp4_builder_init_track(state, state->cur_clip->first_track);
		}

		if (!state->frame_started)
		{
			rc = state->frames_source->start_frame(state->frames_source_context, state->cur_frame, *state->cur_frame_offset);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->frame_started = TRUE;
		}

		// read some data from the frame
		rc = state->frames_source->read(state->frames_source_context, &read_buffer, &read_size, &frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (write_buffer != NULL)
			{
				// flush the write buffer
				rc = state->write_callback(state->write_context, write_buffer, write_buffer_size);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			else if (!state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_builder_frame_writer_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}

		// move to the next frame if done
		if (frame_done)
		{
			state->cur_frame++;
			state->cur_frame_offset++;
			state->frame_started = FALSE;
		}

		if (write_buffer != NULL)
		{
			// if the buffers are contiguous, just increment the size
			if (!state->reuse_buffers && write_buffer + write_buffer_size == read_buffer)
			{
				write_buffer_size += read_size;
				continue;
			}

			// buffers not contiguous, flush the write buffer
			rc = state->write_callback(state->write_context, write_buffer, write_buffer_size);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		// reset the write buffer
		write_buffer = read_buffer;
		write_buffer_size = read_size;
	}
}
