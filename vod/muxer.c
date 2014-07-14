#include "muxer.h"

// from ffmpeg mpegtsenc
#define DEFAULT_PES_HEADER_FREQ 16
#define DEFAULT_PES_PAYLOAD_SIZE (((DEFAULT_PES_HEADER_FREQ - 1) * 184 + 170) * 2)			// x2 to match akamai

static int 
compare_streams(const void *s1, const void *s2)
{
	return ((muxer_stream_state_t*)s1)->media_type - ((muxer_stream_state_t*)s2)->media_type;
}

vod_status_t 
muxer_init(
	muxer_state_t* state, 
	request_context_t* request_context,
	int segment_index,
	mpeg_metadata_t* mpeg_metadata, 
	read_cache_state_t* read_cache_state, 
	write_callback_t write_callback, 
	void* write_context,
	bool_t* simulation_supported)
{
	mpegts_encoder_init_streams_state_t init_streams_state;
	mpeg_stream_metadata_t* cur_stream_metadata;
	muxer_stream_state_t* cur_stream;
	uint32_t duration_limit;
	int64_t video_duration = 0;
	unsigned i;
	vod_status_t rc;

	*simulation_supported = TRUE;

	state->request_context = request_context;

	rc = mpegts_encoder_init(&state->mpegts_encoder_state, request_context, segment_index, write_callback, write_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	state->first_stream = vod_alloc(request_context->pool, sizeof(*state->first_stream) * mpeg_metadata->streams.nelts);
	if (state->first_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"muxer_init: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->last_stream = state->first_stream + mpeg_metadata->streams.nelts;

	state->read_cache_state = read_cache_state;
	state->cur_frame = NULL;

	cur_stream = state->first_stream;
	for (i = 0; i < mpeg_metadata->streams.nelts; i++, cur_stream++)
	{
		cur_stream_metadata = (mpeg_stream_metadata_t*)(mpeg_metadata->streams.elts) + i;

		cur_stream->media_type = cur_stream_metadata->media_type;
		cur_stream->first_frame = cur_stream_metadata->frames;
		cur_stream->cur_frame = cur_stream_metadata->frames;
		cur_stream->last_frame = cur_stream->cur_frame + cur_stream_metadata->frame_count;
		cur_stream->first_frame_offset = cur_stream_metadata->frame_offsets;
		cur_stream->cur_frame_offset = cur_stream_metadata->frame_offsets;
		cur_stream->cc = 0;
		cur_stream->output_frame.cc = &cur_stream->cc;
		cur_stream->output_frame.last_stream_frame = FALSE;

		switch (cur_stream_metadata->media_type)
		{
		case MEDIA_TYPE_VIDEO:
			if (cur_stream_metadata->duration > video_duration)
			{
				video_duration = cur_stream_metadata->duration;
			}

			cur_stream->buffer_state = NULL;
			cur_stream->top_filter_context = vod_alloc(request_context->pool, sizeof(mp4_to_annexb_state_t));
			if (cur_stream->top_filter_context == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"muxer_init: vod_alloc failed (2)");
				return VOD_ALLOC_FAILED;
			}

			rc = mp4_to_annexb_init(
				cur_stream->top_filter_context,
				request_context,
				&mpegts_encoder,
				&state->mpegts_encoder_state,
				cur_stream_metadata->extra_data,
				cur_stream_metadata->extra_data_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (!mp4_to_annexb_simulation_supported(cur_stream->top_filter_context))
			{
				*simulation_supported = FALSE;
			}

			cur_stream->top_filter = &mp4_to_annexb;
			break;

		case MEDIA_TYPE_AUDIO:
			cur_stream->buffer_state = vod_alloc(request_context->pool, sizeof(buffer_filter_t));
			if (cur_stream->buffer_state == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"muxer_init: vod_alloc failed (3)");
				return VOD_ALLOC_FAILED;
			}

			rc = buffer_filter_init(cur_stream->buffer_state, request_context, &mpegts_encoder, &state->mpegts_encoder_state, DEFAULT_PES_PAYLOAD_SIZE);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_stream->top_filter_context = vod_alloc(request_context->pool, sizeof(adts_encoder_state_t));
			if (cur_stream->top_filter_context == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"muxer_init: vod_alloc failed (4)");
				return VOD_ALLOC_FAILED;
			}

			rc = adts_encoder_init(
				cur_stream->top_filter_context, 
				request_context, 
				&buffer_filter, 
				cur_stream->buffer_state, 
				cur_stream_metadata->extra_data, 
				cur_stream_metadata->extra_data_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_stream->top_filter = &adts_encoder;
			break;
		}
	}

	// place the video streams before the audio streams (usually there will only one video and one audio)
	qsort(state->first_stream, state->last_stream - state->first_stream, sizeof(*state->first_stream), compare_streams);

	// init the packetizer streams and get the packet ids / stream ids
	rc = mpegts_encoder_init_streams(&state->mpegts_encoder_state, &init_streams_state, segment_index);
	if (rc != VOD_OK)
	{
		return rc;
	}

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		rc = mpegts_encoder_add_stream(
			&init_streams_state, 
			cur_stream->media_type, 
			&cur_stream->output_frame.pid, 
			&cur_stream->output_frame.sid);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	mpegts_encoder_finalize_streams(&init_streams_state);

	video_duration /= 90;		// convert to millis
	duration_limit = request_context->end - request_context->start;
	state->video_duration = MIN(video_duration, duration_limit);

	return VOD_OK;
}

static muxer_stream_state_t* 
muxer_choose_stream(muxer_state_t* state)
{
	muxer_stream_state_t* cur_stream;
	muxer_stream_state_t* result = NULL;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->cur_frame >= cur_stream->last_frame)
		{
			continue;
		}

		if (result == NULL || cur_stream->cur_frame->dts < result->cur_frame->dts)
		{
			result = cur_stream;
		}
	}

	return result;
}

static vod_status_t 
muxer_start_frame(muxer_state_t* state)
{
	muxer_stream_state_t* cur_stream;
	muxer_stream_state_t* selected_stream;
	output_frame_t* output_frame;
	int64_t buffer_dts;
	vod_status_t rc;

	selected_stream = muxer_choose_stream(state);
	if (selected_stream == NULL)
	{
		return VOD_OK;		// done
	}

	// init the frame
	state->cur_frame = selected_stream->cur_frame;
	selected_stream->cur_frame++;
	state->cur_frame_offset = *selected_stream->cur_frame_offset;
	selected_stream->cur_frame_offset++;

	// flush any buffered frames if their delay becomes too big
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream || cur_stream->buffer_state == NULL)
		{
			continue;
		}

		if (buffer_filter_get_dts(cur_stream->buffer_state, &buffer_dts) &&
			state->cur_frame->dts > buffer_dts + HLS_DELAY)
		{
			rc = buffer_filter_force_flush(cur_stream->buffer_state);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}

	// set the current top_filter
	state->cur_writer = selected_stream->top_filter;
	state->cur_writer_context = selected_stream->top_filter_context;

	// choose the base mpegts frame info
	output_frame = &selected_stream->output_frame;
	if (selected_stream->cur_frame >= selected_stream->last_frame)
	{
		output_frame->last_stream_frame = TRUE;
	}

	// initialize the mpeg ts frame info
	output_frame->pts = state->cur_frame->pts;
	output_frame->dts = state->cur_frame->dts;
	output_frame->key = state->cur_frame->key_frame;
	output_frame->original_size = state->cur_frame->size;

	state->cache_slot_id = output_frame->pid;

	// start the frame
	rc = state->cur_writer->start_frame(state->cur_writer_context, output_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	state->cur_frame_pos = 0;

	return VOD_OK;
}
		
vod_status_t 
muxer_process(muxer_state_t* state, uint64_t* required_offset)
{
	muxer_stream_state_t* cur_stream;
	u_char* read_buffer;
	uint32_t read_size;
	uint32_t write_size;
	uint64_t offset;
	vod_status_t rc;

	for (;;)
	{
		// start a new frame if we don't have a frame
		if (state->cur_frame == NULL)
		{
			rc = muxer_start_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			if (state->cur_frame == NULL)
			{
				for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
				{
					if (cur_stream->buffer_state == NULL)
					{
						continue;
					}

					rc = buffer_filter_force_flush(cur_stream->buffer_state);
					if (rc != VOD_OK)
					{
						return rc;
					}
				}

				rc = mpegts_encoder_flush(&state->mpegts_encoder_state);
				if (rc != VOD_OK)
				{
					return rc;
				}

				return VOD_OK;
			}
		}
		
		// read some data from the frame
		offset = state->cur_frame_offset + state->cur_frame_pos;
		if (!read_cache_get_from_cache(state->read_cache_state, state->cache_slot_id, offset, &read_buffer, &read_size))
		{
			*required_offset = offset;
			return VOD_AGAIN;
		}
		
		// write the frame
		write_size = MIN(state->cur_frame->size - state->cur_frame_pos, read_size);
		rc = state->cur_writer->write(state->cur_writer_context, read_buffer, write_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_frame_pos += write_size;
		
		// flush the frame if we finished writing it
		if (state->cur_frame_pos >= state->cur_frame->size)
		{
			rc = state->cur_writer->flush_frame(state->cur_writer_context, 0);
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			state->cur_frame = NULL;
			continue;
		}
	}
}

static void 
muxer_simulation_flush(muxer_state_t* state)
{
	muxer_stream_state_t* cur_stream;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->buffer_state == NULL)
		{
			continue;
		}

		buffer_filter_simulated_force_flush(cur_stream->buffer_state);
	}
}

static void 
muxer_simulation_flush_delayed_streams(muxer_state_t* state, muxer_stream_state_t* selected_stream, int64_t frame_dts)
{
	muxer_stream_state_t* cur_stream;
	int64_t buffer_dts;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream || cur_stream->buffer_state == NULL)
		{
			continue;
		}

		if (buffer_filter_get_dts(cur_stream->buffer_state, &buffer_dts) &&
			frame_dts > buffer_dts + HLS_DELAY)
		{
			buffer_filter_simulated_force_flush(cur_stream->buffer_state);
		}
	}
}

static void 
muxer_simulation_write_frame(muxer_stream_state_t* selected_stream, input_frame_t* cur_frame, bool_t last_frame)
{
	output_frame_t* output_frame;

	output_frame = &selected_stream->output_frame;
	output_frame->last_stream_frame = (
		selected_stream->cur_frame >= selected_stream->last_frame ||
		last_frame);

	// initialize the mpeg ts frame info
	output_frame->pts = cur_frame->pts;
	output_frame->dts = cur_frame->dts;
	output_frame->key = cur_frame->key_frame;
	output_frame->original_size = cur_frame->size;

	selected_stream->top_filter->simulated_write(selected_stream->top_filter_context, output_frame);
}

void 
muxer_simulate_get_iframes(muxer_state_t* state, uint32_t segment_duration, get_iframe_positions_callback_t callback, void* context)
{
	muxer_stream_state_t* selected_stream;
	input_frame_t* cur_frame;
	uint32_t cur_frame_time;
	uint32_t cur_frame_start;
	uint32_t frame_start = 0;
	uint32_t frame_size = 0;
	uint32_t frame_start_time = 0;
	uint32_t first_frame_time = 0;
	uint32_t end_time;
	int64_t segment_end_dts;
	int frame_segment_index = 0;
	int segment_index = 1;

	segment_duration *= 90;			// convert to 90KHz
	segment_end_dts = segment_duration;

	mpegts_encoder_simulated_start_segment(&state->mpegts_encoder_state);

	for (;;)
	{
		// get a frame
		selected_stream = muxer_choose_stream(state);
		if (selected_stream == NULL)
		{
			break;		// done
		}

		cur_frame = selected_stream->cur_frame;
		selected_stream->cur_frame++;

		// check whether we completed a segment
		if (cur_frame->dts >= segment_end_dts)
		{
			// flush all buffered frames
			muxer_simulation_flush(state);

			mpegts_encoder_simulated_start_segment(&state->mpegts_encoder_state);
			segment_index++;
			segment_end_dts += segment_duration;
		}

		// flush any buffered frames if their delay becomes too big
		muxer_simulation_flush_delayed_streams(state, selected_stream, cur_frame->dts);

		cur_frame_start = mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state);

		muxer_simulation_write_frame(
			selected_stream,
			cur_frame,
			selected_stream->cur_frame->dts >= segment_end_dts);

#if (VOD_DEBUG)
		if (cur_frame_start != mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state))
		{
			vod_log_debug4(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"muxer_simulate_get_iframes: wrote frame segment %i packets %uD-%uD pts %L",
				segment_index,
				cur_frame_start / MPEGTS_PACKET_SIZE + 1,
				mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state) / MPEGTS_PACKET_SIZE + 1,
				cur_frame->pts);
		}
#endif // VOD_DEBUG

		if (selected_stream->media_type == MEDIA_TYPE_VIDEO && cur_frame->key_frame)
		{
			cur_frame_time = cur_frame->pts / 90;		// convert to millis
			if (frame_size != 0)
			{
				callback(context, frame_segment_index, cur_frame_time - frame_start_time, frame_start, frame_size);
			}
			else
			{
				first_frame_time = cur_frame_time;
			}

			// output segment_index, ts_segment_offset
			frame_start = cur_frame_start;
			frame_size = mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state) - cur_frame_start;
			frame_start_time = cur_frame_time;
			frame_segment_index = segment_index;
		}
	}

	muxer_simulation_flush(state);

	end_time = first_frame_time + state->video_duration;
	if (frame_size != 0 && end_time > frame_start_time)
	{
		callback(context, frame_segment_index, end_time - frame_start_time, frame_start, frame_size);
	}
}

uint32_t 
muxer_simulate_get_segment_size(muxer_state_t* state)
{
	muxer_stream_state_t* selected_stream;
	input_frame_t* cur_frame;
#if (VOD_DEBUG)
	uint32_t cur_frame_start;
#endif

	mpegts_encoder_simulated_start_segment(&state->mpegts_encoder_state);

	for (;;)
	{
		// get a frame
		selected_stream = muxer_choose_stream(state);
		if (selected_stream == NULL)
		{
			break;		// done
		}

		cur_frame = selected_stream->cur_frame;
		selected_stream->cur_frame++;

		// flush any buffered frames if their delay becomes too big
		muxer_simulation_flush_delayed_streams(state, selected_stream, cur_frame->dts);

#if (VOD_DEBUG)
		cur_frame_start = mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state);
#endif

		// write the frame
		muxer_simulation_write_frame(selected_stream, cur_frame, FALSE);

#if (VOD_DEBUG)
		if (cur_frame_start != mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state))
		{
			vod_log_debug3(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"muxer_simulate_get_segment_size: wrote frame in packets %uD-%uD, pts %L",
				cur_frame_start / MPEGTS_PACKET_SIZE + 1,
				mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state) / MPEGTS_PACKET_SIZE + 1,
				cur_frame->pts);
		}
#endif
	}

	// flush all streams
	muxer_simulation_flush(state);

	return mpegts_encoder_simulated_get_offset(&state->mpegts_encoder_state);
}

void 
muxer_simulation_reset(muxer_state_t* state)
{
	muxer_stream_state_t* cur_stream;

	state->cur_frame = NULL;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		cur_stream->cur_frame = cur_stream->first_frame;
		cur_stream->cur_frame_offset = cur_stream->first_frame_offset;
		cur_stream->cc = 0;
		cur_stream->output_frame.last_stream_frame = FALSE;
	}
}
