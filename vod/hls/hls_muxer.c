#include "../input/frames_source_memory.h"
#include "../input/frames_source_cache.h"
#include "frame_joiner_filter.h"
#include "id3_encoder_filter.h"
#include "hls_muxer.h"

#if (VOD_HAVE_OPENSSL_EVP)
#include "frame_encrypt_filter.h"
#include "eac3_encrypt_filter.h"
#endif // VOD_HAVE_OPENSSL_EVP

// from ffmpeg mpegtsenc
#define DEFAULT_PES_HEADER_FREQ 16
#define DEFAULT_PES_PAYLOAD_SIZE ((DEFAULT_PES_HEADER_FREQ - 1) * 184 + 170)

#define hls_rescale_millis(millis) ((millis) * (HLS_TIMESCALE / 1000))

// typedefs
typedef struct {
	media_track_t track;
	input_frame_t frame;
} id3_track_t;

struct id3_context_s {
	id3_encoder_state_t encoder;
	id3_track_t* first_track;
	id3_track_t* cur_track;
};

// forward decls
static vod_status_t hls_muxer_start_frame(hls_muxer_state_t* state);
static vod_status_t hls_muxer_simulate_get_segment_size(hls_muxer_state_t* state, size_t* result);
static void hls_muxer_simulation_reset(hls_muxer_state_t* state);
static vod_status_t hls_muxer_choose_stream(hls_muxer_state_t* state, hls_muxer_stream_state_t** result);

static vod_status_t
hls_muxer_init_track(
	hls_muxer_state_t* state,
	hls_muxer_stream_state_t* cur_stream,
	media_track_t* track)
{
	vod_status_t rc;

	cur_stream->media_type = track->media_info.media_type;
	cur_stream->first_frame_part = &track->frames;
	cur_stream->cur_frame_part = track->frames;
	cur_stream->cur_frame = track->frames.first_frame;
	cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);
	cur_stream->first_frame_time_offset = hls_rescale_millis(track->clip_start_time) + track->first_frame_time_offset;
	cur_stream->clip_from_frame_offset = track->clip_from_frame_offset;

	switch (track->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		rc = mp4_to_annexb_set_media_info(
			&cur_stream->filter_context, 
			&track->media_info);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (state->align_pts)
		{
			cur_stream->first_frame_time_offset -= vod_min(INITIAL_DTS, track->media_info.u.video.initial_pts_delay);
		}
		break;

	case MEDIA_TYPE_AUDIO:
		if (track->media_info.codec_id == VOD_CODEC_ID_AAC)
		{
			rc = adts_encoder_set_media_info(
				&cur_stream->filter_context,
				&track->media_info);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		break;
	}

	cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;

	return VOD_OK;
}

static bool_t
hls_muxer_simulation_supported(
	media_set_t* media_set,
	hls_encryption_params_t* encryption_params)
{
	media_track_t* track;

	/* In sample AES every encrypted NAL unit has to go through emulation prevention, so it is not
	possible to know the exact size of the unit in advance */
	if (encryption_params->type == HLS_ENC_SAMPLE_AES)
	{
		return FALSE;
	}

	for (track = media_set->filtered_tracks; track < media_set->filtered_tracks_end; track++)
	{
		if (track->media_info.media_type != MEDIA_TYPE_VIDEO)
		{
			continue;
		}

		if (!mp4_to_annexb_simulation_supported(&track->media_info))
		{
			return FALSE;
		}
	}

	return TRUE;
}

static vod_status_t
hls_muxer_init_stream(
	hls_muxer_state_t* state,
	hls_mpegts_muxer_conf_t* conf,
	hls_muxer_stream_state_t* stream,
	media_track_t* track,
	mpegts_encoder_init_streams_state_t* init_streams_state)
{
	vod_status_t rc;

	stream->segment_limit = ULLONG_MAX;
	stream->filter_context.request_context = state->request_context;
	stream->filter_context.context[MEDIA_FILTER_MPEGTS] = &stream->mpegts_encoder_state;
	stream->filter_context.context[MEDIA_FILTER_BUFFER] = NULL;

	rc = mpegts_encoder_init(
		&stream->filter,
		&stream->mpegts_encoder_state,
		init_streams_state,
		track,
		&state->queue,
		conf->interleave_frames,
		conf->align_frames);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
hls_muxer_init_id3_stream(
	hls_muxer_state_t* state,
	hls_mpegts_muxer_conf_t* conf,
	media_set_t* media_set,
	mpegts_encoder_init_streams_state_t* init_streams_state)
{
	hls_muxer_stream_state_t* cur_stream;
	media_track_t* dest_track;
	media_track_t* ref_track;
	id3_context_t* context;
	id3_track_t* last_track;
	id3_track_t* cur_track;
	vod_status_t rc;
	bool_t frame_added;
	void* frames_source_context;

	cur_stream = state->last_stream;

	rc = hls_muxer_init_stream(
		state,
		conf,
		cur_stream,
		NULL,
		init_streams_state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (conf->id3_data.len <= 0)
	{
		state->id3_context = NULL;
		return VOD_OK;
	}

	// allocate the context
	context = vod_alloc(state->request_context->pool, 
		sizeof(*context) + 
		(sizeof(context->first_track[0])) * media_set->clip_count);
	if (context == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"hls_muxer_init_id3_stream: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_track = (void*)(context + 1);
	last_track = cur_track + media_set->clip_count;

	// init the memory frames source
	rc = frames_source_memory_init(state->request_context, &frames_source_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// init the tracks
	context->first_track = cur_track;
	ref_track = media_set->filtered_tracks;
	frame_added = FALSE;

	for (; cur_track < last_track; cur_track++, ref_track += media_set->total_track_count)
	{
		// init the track
		dest_track = &cur_track->track;
		dest_track->media_info.media_type = MEDIA_TYPE_NONE;
		dest_track->clip_start_time = ref_track->clip_start_time;
		dest_track->first_frame_time_offset = ref_track->first_frame_time_offset;
		dest_track->clip_from_frame_offset = ref_track->clip_from_frame_offset;

		// init the frame part
		dest_track->frames.next = NULL;
		dest_track->frames.first_frame = &cur_track->frame;
		dest_track->frames.last_frame = &cur_track->frame;
		if (ref_track->frame_count > 0 && !frame_added)
		{
			frame_added = TRUE;
			dest_track->frames.last_frame++;
		}
		dest_track->frames.frames_source = &frames_source_memory;
		dest_track->frames.frames_source_context = frames_source_context;

		// init the frame
		cur_track->frame.offset = (uintptr_t)conf->id3_data.data;
		cur_track->frame.size = conf->id3_data.len;
		cur_track->frame.duration = 0;
		cur_track->frame.key_frame = 1;
		cur_track->frame.pts_delay = 0;
	}

	// init the first track
	rc = hls_muxer_init_track(state, cur_stream, &context->first_track[0].track);
	if (rc != VOD_OK)
	{
		return rc;
	}

	context->cur_track = context->first_track + 1;

	// init the id3 encoder
	id3_encoder_init(&context->encoder, &cur_stream->filter, &cur_stream->filter_context);

	// update the state
	state->last_stream++;
	state->id3_context = context;

	return VOD_OK;
}

static vod_status_t 
hls_muxer_init_base(
	hls_muxer_state_t* state,
	request_context_t* request_context,
	hls_mpegts_muxer_conf_t* conf,
	hls_encryption_params_t* encryption_params,
	uint32_t segment_index,
	media_set_t* media_set,
	bool_t* simulation_supported,
	vod_str_t* response_header)
{
	mpegts_encoder_init_streams_state_t init_streams_state;
	media_track_t* track;
	hls_muxer_stream_state_t* cur_stream;
	vod_status_t rc;

	*simulation_supported = hls_muxer_simulation_supported(media_set, encryption_params);

	state->request_context = request_context;
	state->align_pts = conf->align_pts;
	state->cur_frame = NULL;
	state->video_duration = 0;
	state->first_time = TRUE;

	state->media_set = media_set;
	state->use_discontinuity = media_set->use_discontinuity;

	// init the packetizer streams and get the packet ids / stream ids
	rc = mpegts_encoder_init_streams(
		request_context,
		encryption_params,
		&init_streams_state,
		segment_index);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate the streams
	state->first_stream = vod_alloc(request_context->pool, 
		sizeof(*state->first_stream) * (media_set->total_track_count + 1));
	if (state->first_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hls_muxer_init_base: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->last_stream = state->first_stream + media_set->total_track_count;

	track = media_set->filtered_tracks;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, track++)
	{
		rc = hls_muxer_init_stream(
			state,
			conf,
			cur_stream,
			track,
			&init_streams_state);
		if (rc != VOD_OK)
		{
			return rc;
		}

		switch (track->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			switch (media_set->segmenter_conf->manifest_duration_policy)
			{
			case MDP_MAX:
				if (track->media_info.duration_millis > state->video_duration)
				{
					state->video_duration = track->media_info.duration_millis;
				}
				break;

			case MDP_MIN:
				if (track->media_info.duration_millis > 0 &&
					(state->video_duration == 0 || track->media_info.duration_millis < state->video_duration))
				{
					state->video_duration = track->media_info.duration_millis;
				}
				break;
			}

			rc = mp4_to_annexb_init(
				&cur_stream->filter,
				&cur_stream->filter_context,
				encryption_params);
			if (rc != VOD_OK)
			{
				return rc;
			}
			break;

		case MEDIA_TYPE_AUDIO:
			if (conf->interleave_frames)
			{
				// frame interleaving enabled, just join several audio frames according to timestamp
				rc = frame_joiner_init(
					&cur_stream->filter,
					&cur_stream->filter_context);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			else
			{
				// no frame interleaving, buffer the audio until it reaches a certain size / delay from video
				rc = buffer_filter_init(
					&cur_stream->filter,
					&cur_stream->filter_context,
					conf->align_frames,
					DEFAULT_PES_PAYLOAD_SIZE);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

			if (track->media_info.codec_id == VOD_CODEC_ID_AAC)
			{
				rc = adts_encoder_init(
					&cur_stream->filter,
					&cur_stream->filter_context);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

#if (VOD_HAVE_OPENSSL_EVP)
			if (encryption_params->type == HLS_ENC_SAMPLE_AES)
			{
				switch (track->media_info.codec_id)
				{
				case VOD_CODEC_ID_AAC:
				case VOD_CODEC_ID_AC3:
				case VOD_CODEC_ID_EAC3:
					break;

				default:
					vod_log_error(VOD_LOG_ERR, request_context->log, 0,
						"hls_muxer_init_base: sample aes encryption is supported only for aac/ac3/eac3");
					return VOD_BAD_REQUEST;
				}

				rc = frame_encrypt_filter_init(
					&cur_stream->filter,
					&cur_stream->filter_context,
					encryption_params);
				if (rc != VOD_OK)
				{
					return rc;
				}

				if (track->media_info.codec_id == VOD_CODEC_ID_EAC3)
				{
					rc = eac3_encrypt_filter_init(
						&cur_stream->filter,
						&cur_stream->filter_context);
					if (rc != VOD_OK)
					{
						return rc;
					}
				}
			}
#endif // VOD_HAVE_OPENSSL_EVP
			break;
		}

		rc = hls_muxer_init_track(state, cur_stream, track);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	state->first_clip_track = track;

	// init the id3 stream
	rc = hls_muxer_init_id3_stream(state, conf, media_set, &init_streams_state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	mpegts_encoder_finalize_streams(&init_streams_state, response_header);

	if (media_set->timing.durations != NULL)
	{
		state->video_duration = media_set->timing.total_duration;
	}

	return VOD_OK;
}

vod_status_t
hls_muxer_init_segment(
	request_context_t* request_context,
	hls_mpegts_muxer_conf_t* conf,
	hls_encryption_params_t* encryption_params,
	uint32_t segment_index,
	media_set_t* media_set,
	write_callback_t write_callback,
	void* write_context,
	bool_t reuse_buffers,
	size_t* response_size,
	vod_str_t* response_header,
	hls_muxer_state_t** processor_state)
{
	hls_muxer_state_t* state;
	bool_t simulation_supported;
	vod_status_t rc;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hls_muxer_init_segment: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// init the write queue
	write_buffer_queue_init(
		&state->queue,
		request_context,
		write_callback,
		write_context,
		reuse_buffers);

	rc = hls_muxer_init_base(
		state, 
		request_context, 
		conf, 
		encryption_params, 
		segment_index, 
		media_set, 
		&simulation_supported,
		response_header);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (simulation_supported)
	{
		rc = hls_muxer_simulate_get_segment_size(state, response_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		hls_muxer_simulation_reset(state);
	}

	rc = hls_muxer_start_frame(state);
	if (rc != VOD_OK)
	{
		if (rc != VOD_NOT_FOUND)
		{
			return rc;
		}

		*processor_state = NULL;		// no frames, nothing to do
	}
	else
	{
		*processor_state = state;
	}

	return VOD_OK;
}

static vod_status_t
hls_muxer_reinit_tracks(hls_muxer_state_t* state)
{
	media_track_t* track;
	hls_muxer_stream_state_t* cur_stream;
	vod_status_t rc;

	state->first_time = TRUE;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->media_type != MEDIA_TYPE_NONE)		// id3 track
		{
			track = state->first_clip_track++;
		}
		else
		{
			track = &state->id3_context->cur_track->track;
			state->id3_context->cur_track++;
		}

		rc = hls_muxer_init_track(state, cur_stream, track);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t
hls_muxer_choose_stream(hls_muxer_state_t* state, hls_muxer_stream_state_t** result)
{
	hls_muxer_stream_state_t* cur_stream;
	hls_muxer_stream_state_t* min_dts = NULL;
	vod_status_t rc;
	bool_t has_frames = FALSE;

	for (;;)
	{
		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			if (cur_stream->cur_frame >= cur_stream->cur_frame_part.last_frame)
			{
				if (cur_stream->cur_frame_part.next == NULL)
				{
					continue;
				}
				cur_stream->cur_frame_part = *cur_stream->cur_frame_part.next;
				cur_stream->cur_frame = cur_stream->cur_frame_part.first_frame;
				cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);
				state->first_time = TRUE;
			}

			has_frames = TRUE;

			if (cur_stream->next_frame_time_offset >= cur_stream->segment_limit)
			{
				continue;
			}

			if (min_dts == NULL || cur_stream->next_frame_time_offset < min_dts->next_frame_time_offset)
			{
				min_dts = cur_stream;
			}
		}

		if (min_dts != NULL)
		{
			*result = min_dts;
			return VOD_OK;
		}

		if (state->first_clip_track >= state->media_set->filtered_tracks_end || has_frames)
		{
			break;
		}

		rc = hls_muxer_reinit_tracks(state);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (state->use_discontinuity)
		{
			break;
		}
	}

	return VOD_NOT_FOUND;
}

static vod_status_t 
hls_muxer_start_frame(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	hls_muxer_stream_state_t* selected_stream;
	read_cache_hint_t cache_hint;
	output_frame_t output_frame;
	input_frame_t* cur_frame;
	uint64_t cur_frame_time_offset;
	uint64_t cur_frame_dts;
	uint64_t buffer_dts;
	vod_status_t rc;

	rc = hls_muxer_choose_stream(state, &selected_stream);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// init the frame
	state->cur_frame = selected_stream->cur_frame;
	selected_stream->cur_frame++;
	state->frames_source = selected_stream->cur_frame_part.frames_source;
	state->frames_source_context = selected_stream->cur_frame_part.frames_source_context;
	cur_frame_time_offset = selected_stream->next_frame_time_offset;
	cur_frame_dts = selected_stream->next_frame_time_offset;
	selected_stream->next_frame_time_offset += state->cur_frame->duration;

	// TODO: in the case of multi clip without discontinuity, the test below is not sufficient
	state->last_stream_frame = selected_stream->cur_frame >= selected_stream->cur_frame_part.last_frame && 
		selected_stream->cur_frame_part.next == NULL;

	cache_hint.min_offset = ULLONG_MAX;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream)
		{
			continue;
		}

		// flush any buffered frames if their delay becomes too big
		if (cur_stream->filter_context.context[MEDIA_FILTER_BUFFER] != NULL)
		{
			if (buffer_filter_get_dts(&cur_stream->filter_context, &buffer_dts) &&
				cur_frame_dts > buffer_dts + HLS_DELAY / 2)
			{
				rc = buffer_filter_force_flush(&cur_stream->filter_context, FALSE);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
		}

		// find the min offset
		cur_frame = cur_stream->cur_frame;
		if (cur_frame < cur_stream->cur_frame_part.last_frame &&
			cur_frame->offset < cache_hint.min_offset &&
			cur_stream->source == selected_stream->source)
		{
			cache_hint.min_offset = cur_frame->offset;
			cache_hint.min_offset_slot_id = cur_stream->mpegts_encoder_state.stream_info.pid;
		}
	}

	// set the current top_filter
	state->cur_writer = &selected_stream->filter;
	state->cur_writer_context = &selected_stream->filter_context;

	// initialize the mpeg ts frame info
	output_frame.pts = cur_frame_time_offset + state->cur_frame->pts_delay;
	output_frame.dts = cur_frame_dts;
	output_frame.key = state->cur_frame->key_frame;
	output_frame.size = state->cur_frame->size;
	output_frame.header_size = 0;

	state->cache_slot_id = selected_stream->mpegts_encoder_state.stream_info.pid;

	// start the frame
	rc = state->frames_source->start_frame(state->frames_source_context, state->cur_frame, &cache_hint);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = state->cur_writer->start_frame(state->cur_writer_context, &output_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
hls_muxer_send(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	off_t min_offset = state->queue.cur_offset;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->mpegts_encoder_state.send_queue_offset < min_offset)
		{
			min_offset = cur_stream->mpegts_encoder_state.send_queue_offset;
		}
	}

	return write_buffer_queue_send(&state->queue, min_offset);
}

vod_status_t 
hls_muxer_process(hls_muxer_state_t* state)
{
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;
	bool_t wrote_data = FALSE;
	bool_t frame_done;

	for (;;)
	{	
		// read some data from the frame
		rc = state->frames_source->read(state->frames_source_context, &read_buffer, &read_size, &frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!wrote_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"hls_muxer_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			rc = hls_muxer_send(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->first_time = FALSE;

			return VOD_AGAIN;
		}

		wrote_data = TRUE;
		
		// write the frame
		rc = state->cur_writer->write(state->cur_writer_context, read_buffer, read_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
		
		// if frame not done, try to read more data from the cache
		if (!frame_done)
		{
			continue;
		}

		// flush the frame and start a new one
		rc = state->cur_writer->flush_frame(state->cur_writer_context, state->last_stream_frame);
		if (rc != VOD_OK)
		{
			return rc;
		}
			
		rc = hls_muxer_start_frame(state);
		if (rc != VOD_OK)
		{
			if (rc == VOD_NOT_FOUND)
			{
				break;		// done
			}

			return rc;
		}
	}

	// flush the buffer queue
	rc = write_buffer_queue_flush(&state->queue);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static void 
hls_muxer_simulation_flush_delayed_streams(
	hls_muxer_state_t* state, 
	hls_muxer_stream_state_t* selected_stream, 
	uint64_t frame_dts)
{
	hls_muxer_stream_state_t* cur_stream;
	uint64_t buffer_dts;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream || cur_stream->filter_context.context[MEDIA_FILTER_BUFFER] == NULL)
		{
			continue;
		}

		if (buffer_filter_get_dts(&cur_stream->filter_context, &buffer_dts) &&
			frame_dts > buffer_dts + HLS_DELAY / 2)
		{
			vod_log_debug2(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hls_muxer_simulation_flush_delayed_streams: flushing buffered frames buffer dts %L frame dts %L",
				buffer_dts,
				frame_dts);
			buffer_filter_simulated_force_flush(&cur_stream->filter_context, FALSE);
		}
	}
}

static void 
hls_muxer_simulation_write_frame(hls_muxer_stream_state_t* selected_stream, input_frame_t* cur_frame, uint64_t cur_frame_dts, bool_t last_frame)
{
	output_frame_t output_frame;

	// initialize the mpeg ts frame info
	// Note: no need to initialize the pts or original size
	output_frame.dts = cur_frame_dts;
	output_frame.key = cur_frame->key_frame;
	output_frame.header_size = 0;

	selected_stream->filter.simulated_start_frame(&selected_stream->filter_context, &output_frame);
	selected_stream->filter.simulated_write(&selected_stream->filter_context, cur_frame->size);
	selected_stream->filter.simulated_flush_frame(&selected_stream->filter_context, last_frame);
}

static void 
hls_muxer_simulation_set_segment_limit(
	hls_muxer_state_t* state,
	uint64_t segment_end,
	uint32_t timescale)
{
	hls_muxer_stream_state_t* cur_stream;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		cur_stream->segment_limit = (segment_end * HLS_TIMESCALE) / timescale - cur_stream->clip_from_frame_offset;
		cur_stream->is_first_segment_frame = TRUE;
	}
}

static void
hls_muxer_simulation_set_segment_limit_unlimited(
	hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		cur_stream->segment_limit = ULLONG_MAX;
		cur_stream->is_first_segment_frame = TRUE;
	}
}

vod_status_t
hls_muxer_simulate_get_iframes(
	request_context_t* request_context,
	segment_durations_t* segment_durations,
	hls_mpegts_muxer_conf_t* muxer_conf,
	hls_encryption_params_t* encryption_params,
	media_set_t* media_set,
	hls_get_iframe_positions_callback_t callback,
	void* context)
{
	hls_muxer_stream_state_t* selected_stream;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item;
	hls_muxer_state_t state;
	input_frame_t* cur_frame;
	uint32_t cur_frame_time;
	uint32_t frame_start = 0;
	uint32_t frame_size = 0;
	uint32_t frame_start_time = 0;
	uint32_t first_frame_time = 0;
	uint32_t end_time;
	uint32_t frame_segment_index = 0;
	uint32_t segment_index = 0;
	uint64_t cur_frame_dts;
	uint64_t cur_frame_time_offset;
	uint32_t repeat_count;
	uint64_t segment_end;
	bool_t simulation_supported;
	bool_t last_frame;
	vod_status_t rc;
#if (VOD_DEBUG)
	off_t cur_frame_start;
#endif // VOD_DEBUG
	
	cur_item = segment_durations->items;
	last_item = segment_durations->items + segment_durations->item_count;
	if (cur_item >= last_item)
	{
		return VOD_OK;
	}

	// initialize the muxer
	rc = hls_muxer_init_base(
		&state,
		request_context,
		muxer_conf,
		encryption_params,
		0,
		media_set,
		&simulation_supported, 
		NULL);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (!simulation_supported)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"hls_muxer_simulate_get_iframes: simulation not supported for this file, cant create iframe playlist");
		return VOD_BAD_REQUEST;
	}

	// initialize the repeat count, segment end, and the per stream limit
	repeat_count = cur_item->repeat_count - 1;
	segment_end = cur_item->duration;

	if (repeat_count <= 0 && (cur_item + 1 >= last_item || cur_item[1].discontinuity))
	{
		hls_muxer_simulation_set_segment_limit_unlimited(&state);
	}
	else
	{
		hls_muxer_simulation_set_segment_limit(&state, segment_end, segment_durations->timescale);
	}

	mpegts_encoder_simulated_start_segment(&state.queue);

	for (;;)
	{
		// get a frame
		for (;;)
		{
			// choose a stream for the current frame
			rc = hls_muxer_choose_stream(&state, &selected_stream);
			if (rc == VOD_OK)
			{
				break;
			}

			if (rc != VOD_NOT_FOUND)
			{
				return rc;
			}

			// update the limit for the next segment
			if (repeat_count <= 0)
			{
				cur_item++;
				if (cur_item >= last_item)
				{
					goto done;
				}

				repeat_count = cur_item->repeat_count;
			}

			repeat_count--;
			segment_end += cur_item->duration;

			if (repeat_count <= 0 && (cur_item + 1 >= last_item || cur_item[1].discontinuity))
			{
				hls_muxer_simulation_set_segment_limit_unlimited(&state);
			}
			else
			{
				hls_muxer_simulation_set_segment_limit(&state, segment_end, segment_durations->timescale);
			}

			// start the next segment
			mpegts_encoder_simulated_start_segment(&state.queue);
			segment_index++;
		}

		// update the stream state
		cur_frame = selected_stream->cur_frame;
		selected_stream->cur_frame++;
		cur_frame_time_offset = selected_stream->next_frame_time_offset;
		cur_frame_dts = selected_stream->next_frame_time_offset;
		selected_stream->next_frame_time_offset += cur_frame->duration;
		
		// flush any buffered frames if their delay becomes too big
		hls_muxer_simulation_flush_delayed_streams(&state, selected_stream, cur_frame_dts);

		// check whether this is the last frame of the selected stream in this segment
		last_frame = ((selected_stream->cur_frame >= selected_stream->cur_frame_part.last_frame && 
			selected_stream->cur_frame_part.next == NULL) ||
			selected_stream->next_frame_time_offset >= selected_stream->segment_limit);

		// write the frame
#if (VOD_DEBUG)
		cur_frame_start = state.queue.cur_offset;
#endif // VOD_DEBUG

		hls_muxer_simulation_write_frame(
			selected_stream,
			cur_frame,
			cur_frame_dts,
			last_frame);

#if (VOD_DEBUG)
		if (cur_frame_start != state.queue.cur_offset)
		{
			vod_log_debug4(VOD_LOG_DEBUG_LEVEL, state.request_context->log, 0,
				"hls_muxer_simulate_get_iframes: wrote frame segment %uD packets %uD-%uD dts %L",
				segment_index + 1,
				(uint32_t)(cur_frame_start / MPEGTS_PACKET_SIZE + 1),
				(uint32_t)(state.queue.cur_offset / MPEGTS_PACKET_SIZE + 1),
				cur_frame_dts);
		}
#endif // VOD_DEBUG

		// only care about video key frames
		if (selected_stream->media_type != MEDIA_TYPE_VIDEO)
		{
			continue;
		}

		if (!selected_stream->is_first_segment_frame && selected_stream->prev_key_frame)
		{
			// get the frame time
			cur_frame_time = rescale_time(selected_stream->prev_frame_pts, HLS_TIMESCALE, 1000);		// in millis
			if (frame_size != 0)
			{
				if (cur_frame_time > frame_start_time)
				{
					callback(context, frame_segment_index, cur_frame_time - frame_start_time, frame_start, frame_size);
				}
			}
			else
			{
				first_frame_time = cur_frame_time;
			}

			// save the info of the current keyframe
			frame_start = selected_stream->mpegts_encoder_state.last_frame_start_pos;
			frame_size = selected_stream->mpegts_encoder_state.last_frame_end_pos -
				selected_stream->mpegts_encoder_state.last_frame_start_pos;
			frame_start_time = cur_frame_time;
			frame_segment_index = segment_index;
		}

		if (last_frame && cur_frame[0].key_frame)
		{
			// get the frame time
			cur_frame_time = rescale_time(cur_frame_time_offset + cur_frame[0].pts_delay, HLS_TIMESCALE, 1000);		// in millis
			if (frame_size != 0)
			{
				if (cur_frame_time > frame_start_time)
				{
					callback(context, frame_segment_index, cur_frame_time - frame_start_time, frame_start, frame_size);
				}
			}
			else
			{
				first_frame_time = cur_frame_time;
			}

			// save the info of the current keyframe
			frame_start = selected_stream->mpegts_encoder_state.cur_frame_start_pos;
			frame_size = selected_stream->mpegts_encoder_state.cur_frame_end_pos -
				selected_stream->mpegts_encoder_state.cur_frame_start_pos;
			frame_start_time = cur_frame_time;
			frame_segment_index = segment_index;
		}

		selected_stream->prev_key_frame = cur_frame->key_frame;
		selected_stream->prev_frame_pts = cur_frame_time_offset + cur_frame->pts_delay;
		selected_stream->is_first_segment_frame = FALSE;
	}

done:

	// call the callback for the last frame
	end_time = first_frame_time + state.video_duration;
	if (frame_size != 0 && end_time > frame_start_time)
	{
		callback(context, frame_segment_index, end_time - frame_start_time, frame_start, frame_size);
	}

	return VOD_OK;
}

static vod_status_t 
hls_muxer_simulate_get_segment_size(hls_muxer_state_t* state, size_t* result)
{
	hls_muxer_stream_state_t* selected_stream;
	input_frame_t* cur_frame;
	uint64_t cur_frame_dts;
	off_t segment_size;
	vod_status_t rc;
#if (VOD_DEBUG)
	off_t cur_frame_start;
#endif // VOD_DEBUG

	mpegts_encoder_simulated_start_segment(&state->queue);

	for (;;)
	{
		// get a frame
		rc = hls_muxer_choose_stream(state, &selected_stream);
		if (rc != VOD_OK)
		{
			if (rc == VOD_NOT_FOUND)
			{
				break;		// done
			}
			return rc;
		}

		cur_frame = selected_stream->cur_frame;
		selected_stream->cur_frame++;
		cur_frame_dts = selected_stream->next_frame_time_offset;
		selected_stream->next_frame_time_offset += cur_frame->duration;

		// flush any buffered frames if their delay becomes too big
		hls_muxer_simulation_flush_delayed_streams(state, selected_stream, cur_frame_dts);

#if (VOD_DEBUG)
		cur_frame_start = state->queue.cur_offset;
#endif // VOD_DEBUG
		
		// write the frame
		hls_muxer_simulation_write_frame(
			selected_stream, 
			cur_frame, 
			cur_frame_dts, 
			selected_stream->cur_frame >= selected_stream->cur_frame_part.last_frame && 
				selected_stream->cur_frame_part.next == NULL);

#if (VOD_DEBUG)
		if (cur_frame_start != state->queue.cur_offset)
		{
			vod_log_debug4(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hls_muxer_simulate_get_segment_size: wrote frame in packets %uD-%uD, dts %L, pid %ud",
				(uint32_t)(cur_frame_start / MPEGTS_PACKET_SIZE + 1),
				(uint32_t)(state->queue.cur_offset / MPEGTS_PACKET_SIZE + 1),
				cur_frame_dts, 
				selected_stream->mpegts_encoder_state.stream_info.pid);
		}
#endif // VOD_DEBUG
	}

	segment_size = state->queue.cur_offset;

	*result = segment_size;

	return VOD_OK;
}

static void 
hls_muxer_simulation_reset(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	vod_status_t rc;

	mpegts_encoder_simulated_start_segment(&state->queue);

	if (state->media_set->clip_count > 1)
	{
		state->first_clip_track = state->media_set->filtered_tracks;
		if (state->id3_context != NULL)
		{
			state->id3_context->cur_track = state->id3_context->first_track;
		}
		rc = hls_muxer_reinit_tracks(state);
		if (rc != VOD_OK)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"hls_muxer_simulation_reset: unexpected - hls_muxer_reinit_tracks failed %i", rc);
		}
	}
	else
	{
		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			cur_stream->cur_frame_part = *cur_stream->first_frame_part;
			cur_stream->cur_frame = cur_stream->cur_frame_part.first_frame;
			cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);
			cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
		}
	}

	state->cur_frame = NULL;
}
