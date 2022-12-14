#include "volume_map.h"
#include "audio_filter.h"
#include "audio_decoder.h"
#include "../write_buffer.h"

// constants
#define RMS_LEVEL_PRECISION (100)
#define RMS_LEVEL_FORMAT "%uD.%02uD\n"

// typedefs
typedef struct {
	double sum_squares;
	uint32_t samples;
} volume_map_frame_t;

typedef struct
{
	request_context_t* request_context;
	vod_array_t* frames_array;
	uint32_t timescale;
	int64_t last_pts;
} volume_map_encoder_state_t;

typedef struct {
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	int64_t pts;
} volume_map_frame_reader_state_t;

typedef struct {
	request_context_t* request_context;
	uint32_t interval;

	write_buffer_state_t write_buffer;

	media_track_t* cur_track;
	media_track_t* last_track;
	volume_map_frame_t data;
	int64_t flush_pts;

	volume_map_frame_reader_state_t reader;
	audio_decoder_state_t* decoder;
} volume_map_writer_state_t;

// common
static vod_status_t
volume_map_calc_frame(
	request_context_t* request_context,
	AVFrame* frame,
	volume_map_frame_t* result)
{
	const float** channel_cur;
	const float** channel_end;
	const float* cur;
	const float* end;
	double sum_squares;
	double sample;
	int channels;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	channels = frame->ch_layout.nb_channels;
#else
	channels = frame->channels;
#endif

	switch (frame->format)
	{
	case AV_SAMPLE_FMT_FLTP:
		sum_squares = 0;
		channel_cur = (const float**)frame->extended_data;
		channel_end = channel_cur + channels;
		for (; channel_cur < channel_end; channel_cur++)
		{
			cur = *channel_cur;
			end = cur + frame->nb_samples;
			for (; cur < end; cur++)
			{
				sample = *cur;
				sum_squares += sample * sample;
			}
		}
		break;

	default:
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"volume_map_calc_frame: unsupported sample format %d", frame->format);
		return VOD_UNEXPECTED;
	}

	result->sum_squares = sum_squares;
	result->samples = frame->nb_samples * channels;
	return VOD_OK;
}

// audio filter encoder
vod_status_t
volume_map_encoder_init(
	request_context_t* request_context,
	uint32_t timescale,
	vod_array_t* frames_array,
	void** result)
{
	volume_map_encoder_state_t* state;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"volume_map_encoder_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->frames_array = frames_array;
	state->timescale = timescale;

	*result = state;

	return VOD_OK;
}

vod_status_t
volume_map_encoder_update_media_info(
	void* context,
	media_info_t* media_info)
{
	volume_map_encoder_state_t* state = context;

	media_info->timescale = state->timescale;
	media_info->codec_id = VOD_CODEC_ID_VOLUME_MAP;
	return VOD_OK;
}

vod_status_t
volume_map_encoder_write_frame(
	void* context,
	AVFrame* frame)
{
	volume_map_encoder_state_t* state = context;
	volume_map_frame_t* data;
	input_frame_t* cur_frame;
	vod_status_t rc;
	
	rc = audio_filter_alloc_memory_frame(
		state->request_context,
		state->frames_array,
		sizeof(*data),
		&cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	data = (void*)(uintptr_t)cur_frame->offset;
	rc = volume_map_calc_frame(
		state->request_context, 
		frame, 
		data);
	if (rc != VOD_OK)
	{
		return rc;
	}

	cur_frame->duration = rescale_time(frame->nb_samples, frame->sample_rate, state->timescale);
	cur_frame->pts_delay = 0;

	// update the duration of the previous frame
	if (state->frames_array->nelts > 1 && 
		frame->pts != AV_NOPTS_VALUE &&
		state->last_pts != AV_NOPTS_VALUE)
	{
		cur_frame[-1].duration = frame->pts - state->last_pts;
	}
	state->last_pts = frame->pts;

	av_frame_unref(frame);
	
	return VOD_OK;
}

// memory frame reader
static void
volume_map_frame_reader_init(volume_map_frame_reader_state_t* state, media_track_t* track)
{
	// initialize the frame state
	state->cur_frame_part = track->frames;
	state->cur_frame = track->frames.first_frame;
	state->pts = track->first_frame_time_offset;

	state->cur_frame_part.frames_source->set_cache_slot_id(
		state->cur_frame_part.frames_source_context,
		0);
}

static vod_status_t
volume_map_frame_reader_get_frame(
	volume_map_frame_reader_state_t* state,
	volume_map_frame_t** data,
	int64_t* pts)
{
	if (state->cur_frame >= state->cur_frame_part.last_frame)
	{
		return VOD_DONE;
	}

	*data = (void*)(uintptr_t)state->cur_frame->offset;
	*pts = state->pts;
	state->pts += state->cur_frame->duration;

	// move to the next frame
	state->cur_frame++;
	if (state->cur_frame >= state->cur_frame_part.last_frame &&
		state->cur_frame_part.next != NULL)
	{
		state->cur_frame_part = *state->cur_frame_part.next;
		state->cur_frame = state->cur_frame_part.first_frame;
	}

	return VOD_OK;
}

// writer
static vod_status_t
volume_map_writer_init_track(volume_map_writer_state_t* state)
{
	vod_pool_cleanup_t *cln;
	media_track_t* track = state->cur_track;
	vod_status_t rc;

	if (track->media_info.codec_id == VOD_CODEC_ID_VOLUME_MAP)
	{
		volume_map_frame_reader_init(&state->reader, state->cur_track);
		return VOD_OK;
	}

	// init the decoder
	state->decoder = vod_alloc(state->request_context->pool, sizeof(*state->decoder));
	if (state->decoder == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"volume_map_writer_init_track: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(state->decoder, sizeof(*state->decoder));
	
	cln = vod_pool_cleanup_add(state->request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"volume_map_writer_init_track: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)audio_decoder_free;
	cln->data = state->decoder;
	
	rc = audio_decoder_init(
		state->decoder,
		state->request_context,
		track,
		0);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

vod_status_t
volume_map_writer_init(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t interval,
	write_callback_t write_callback,
	void* write_context,
	void** result)
{
	volume_map_writer_state_t* state;
	vod_status_t rc;

	state = vod_alloc(request_context->pool, sizeof(volume_map_writer_state_t));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"volume_map_writer_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	write_buffer_init(
		&state->write_buffer, 
		request_context, 
		write_callback, 
		write_context, 
		FALSE);

	state->request_context = request_context;
	state->cur_track = media_set->filtered_tracks;
	state->last_track = media_set->filtered_tracks + media_set->clip_count;
	state->flush_pts = LLONG_MIN;
	state->interval = interval;
	state->data.samples = 0;
	state->data.sum_squares = 0;

	rc = volume_map_writer_init_track(state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	*result = state;
	return VOD_OK;
}

static vod_status_t
volume_map_writer_decode_frame(audio_decoder_state_t* state, volume_map_frame_t* data, int64_t* pts)
{
	AVFrame* frame;
	vod_status_t rc;

	rc = audio_decoder_get_frame(state, &frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = volume_map_calc_frame(state->request_context, frame, data);
	if (rc != VOD_OK)
	{
		return rc;
	}

	*pts = frame->pts;

	return VOD_OK;
}

static vod_status_t
volume_map_writer_write_line(volume_map_writer_state_t* state, int64_t pts)
{
	vod_status_t rc;
	int32_t rms_level;
	size_t ignore;
	u_char* start;
	u_char* p;

	rc = write_buffer_get_bytes(
		&state->write_buffer,
		VOD_INT64_LEN + VOD_INT32_LEN * 2 + 3,
		&ignore,
		&start);
	if (rc != VOD_OK)
	{
		return rc;
	}

	p = start;

	p = vod_sprintf(p, "%L,", pts);

	rms_level = (int32_t)(log10(state->data.sum_squares / state->data.samples) * 10 * RMS_LEVEL_PRECISION);

	if (rms_level < 0)
	{
		*p++ = '-';
		rms_level = -rms_level;
	}

	p = vod_sprintf(p, RMS_LEVEL_FORMAT, rms_level / RMS_LEVEL_PRECISION, rms_level % RMS_LEVEL_PRECISION);

	state->write_buffer.cur_pos += p - start;

	return VOD_OK;
}

vod_status_t
volume_map_writer_process(void* context)
{
	volume_map_writer_state_t* state = context;
	volume_map_frame_t data_buf;
	volume_map_frame_t* data = NULL;
	vod_status_t rc;
	int64_t pts = 0;

	for (;;)
	{
		// get a frame
		if (state->cur_track->media_info.codec_id == VOD_CODEC_ID_VOLUME_MAP)
		{
			rc = volume_map_frame_reader_get_frame(&state->reader, &data, &pts);
		}
		else
		{
			data = &data_buf;
			rc = volume_map_writer_decode_frame(state->decoder, &data_buf, &pts);
		}

		if (rc == VOD_DONE)
		{
			// move to the next track
			state->cur_track++;
			if (state->cur_track >= state->last_track)
			{
				return write_buffer_flush(&state->write_buffer, FALSE);
			}

			rc = volume_map_writer_init_track(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
			continue;
		}

		if (rc != VOD_OK)
		{
			return rc;
		}
	
		pts += state->cur_track->clip_start_time;

		if (pts < state->flush_pts)
		{
			state->data.sum_squares += data->sum_squares;
			state->data.samples += data->samples;
			continue;
		}

		// write a line
		if (state->data.samples > 0 && state->data.sum_squares > 0)
		{
			rc = volume_map_writer_write_line(state, pts);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		// update state
		state->data.sum_squares = data->sum_squares;
		state->data.samples = data->samples;
		state->flush_pts += state->interval;
		if (state->flush_pts < pts)
		{
			state->flush_pts = pts + state->interval;
		}
	}
}
