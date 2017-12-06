#include "mp4_muxer.h"
#include "../input/frames_source_cache.h"
#include "../mp4/mp4_defs.h"
#include "../mp4/mp4_fragment.h"

// constants
#define MDAT_HEADER_SIZE (ATOM_HEADER_SIZE)

// macros
#define mp4_rescale_millis(millis, timescale) (millis * ((timescale) / 1000))

// state
typedef struct {
	// fixed
	write_callback_t write_callback;
	void* write_context;
	uint32_t timescale;
	int media_type;
	uint32_t frame_count;
	uint32_t index;

	uint64_t first_frame_time_offset;
	uint64_t next_frame_time_offset;

	// input frames
	frame_list_part_t* first_frame_part;
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	media_clip_source_t* source;
	uint32_t last_frame_size;

	// frame output offsets
	uint32_t* first_frame_output_offset;
	uint32_t* cur_frame_output_offset;
} mp4_muxer_stream_state_t;

struct mp4_muxer_state_s {
	// fixed
	request_context_t* request_context;
	bool_t reuse_buffers;
	media_set_t* media_set;
	bool_t per_stream_writer;

	mp4_muxer_stream_state_t* first_stream;
	mp4_muxer_stream_state_t* last_stream;

	// cur clip state
	media_track_t* first_clip_track;

	mp4_muxer_stream_state_t* selected_stream;
	input_frame_t* cur_frame;
	int cache_slot_id;
	frames_source_t* frames_source;
	void* frames_source_context;
	bool_t first_time;
};

static vod_status_t mp4_muxer_start_frame(mp4_muxer_state_t* state);

// trun write functions
static u_char*
mp4_muxer_write_trun_header(
	u_char* p,
	uint32_t offset,
	uint32_t frame_count,
	uint32_t frame_size,
	uint32_t flags)
{
	size_t atom_size;

	atom_size = ATOM_HEADER_SIZE + sizeof(trun_atom_t) + frame_size * frame_count;

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, flags);				// flags
	write_be32(p, frame_count);			// frame count
	write_be32(p, offset);				// offset from mdat start to frame raw data (excluding the tag)

	return p;
}

static u_char*
mp4_muxer_write_video_trun_frame(u_char* p, input_frame_t* frame, uint32_t initial_pts_delay)
{
	int32_t pts_delay = frame->pts_delay - initial_pts_delay;

	write_be32(p, frame->duration);
	write_be32(p, frame->size);
	if (frame->key_frame)
	{
		write_be32(p, 0x02000000);		// I-frame
	}
	else
	{
		write_be32(p, 0x01010000);		// not I-frame + non key sample
	}
	write_be32(p, pts_delay);
	return p;
}

static u_char*
mp4_muxer_write_audio_trun_frame(u_char* p, input_frame_t* frame)
{
	write_be32(p, frame->duration);
	write_be32(p, frame->size);
	return p;
}

static u_char*
mp4_muxer_write_video_trun_atoms(
	u_char* p,
	media_set_t* media_set,
	mp4_muxer_stream_state_t* cur_stream,
	uint32_t base_offset)
{
	media_track_t* cur_track;
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t initial_pts_delay;
	uint32_t* output_offset = cur_stream->first_frame_output_offset;
	uint32_t clip_index;
	uint32_t start_offset = 0;
	uint32_t cur_offset = UINT_MAX;
	uint32_t frame_count = 0;
	u_char* trun_header = NULL;

	clip_index = 0;
	cur_track = media_set->filtered_tracks + cur_stream->index;
	initial_pts_delay = cur_track->media_info.u.video.initial_pts_delay;
	for (;;)
	{
		part = &cur_track->frames;
		last_frame = part->last_frame;
		for (cur_frame = part->first_frame; ;
			cur_frame++, output_offset++)
		{
			if (cur_frame >= last_frame)
			{
				if (part->next == NULL)
				{
					break;
				}
				part = part->next;
				cur_frame = part->first_frame;
				last_frame = part->last_frame;
			}

			if (*output_offset != cur_offset)
			{
				if (trun_header != NULL)
				{
					// close current trun atom
					mp4_muxer_write_trun_header(
						trun_header, 
						base_offset + start_offset, 
						frame_count, 
						sizeof(trun_video_frame_t), 
						(1 << 24) | TRUN_VIDEO_FLAGS);		// version = 1
				}

				// start a new trun atom
				trun_header = p;
				p += ATOM_HEADER_SIZE + sizeof(trun_atom_t);
				cur_offset = start_offset = *output_offset;
				frame_count = 0;
			}

			// add the frame to the trun atom
			p = mp4_muxer_write_video_trun_frame(p, cur_frame, initial_pts_delay);
			frame_count++;
			cur_offset += cur_frame->size;
		}

		clip_index++;
		if (clip_index >= media_set->clip_count)
		{
			break;
		}
		cur_track += media_set->total_track_count;
		initial_pts_delay = cur_track->media_info.u.video.initial_pts_delay;
	}

	if (trun_header != NULL)
	{
		// close current trun atom
		mp4_muxer_write_trun_header(
			trun_header,
			base_offset + start_offset,
			frame_count,
			sizeof(trun_video_frame_t),
			(1 << 24) | TRUN_VIDEO_FLAGS);		// version = 1
	}

	return p;
}

static u_char*
mp4_muxer_write_audio_trun_atoms(
	u_char* p,
	media_set_t* media_set,
	mp4_muxer_stream_state_t* cur_stream,
	uint32_t base_offset)
{
	media_track_t* cur_track;
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t* output_offset = cur_stream->first_frame_output_offset;
	uint32_t clip_index;
	uint32_t start_offset = 0;
	uint32_t cur_offset = UINT_MAX;
	uint32_t frame_count = 0;
	u_char* trun_header = NULL;

	clip_index = 0;
	cur_track = media_set->filtered_tracks + cur_stream->index;
	for (;;)
	{
		part = &cur_track->frames;
		last_frame = part->last_frame;
		for (cur_frame = part->first_frame; ;
			cur_frame++, output_offset++)
		{
			if (cur_frame >= last_frame)
			{
				if (part->next == NULL)
				{
					break;
				}
				part = part->next;
				cur_frame = part->first_frame;
				last_frame = part->last_frame;
			}

			if (*output_offset != cur_offset)
			{
				if (trun_header != NULL)
				{
					// close current trun atom
					mp4_muxer_write_trun_header(
						trun_header,
						base_offset + start_offset,
						frame_count,
						sizeof(trun_audio_frame_t),
						TRUN_AUDIO_FLAGS);
				}

				// add the frame to the trun atom
				trun_header = p;
				p += ATOM_HEADER_SIZE + sizeof(trun_atom_t);
				cur_offset = start_offset = *output_offset;
				frame_count = 0;
			}

			// add the frame to the trun atom
			p = mp4_muxer_write_audio_trun_frame(p, cur_frame);
			frame_count++;
			cur_offset += cur_frame->size;
		}

		clip_index++;
		if (clip_index >= media_set->clip_count)
		{
			break;
		}
		cur_track += media_set->total_track_count;
	}

	if (trun_header != NULL)
	{
		// close current trun atom
		mp4_muxer_write_trun_header(
			trun_header,
			base_offset + start_offset,
			frame_count,
			sizeof(trun_audio_frame_t),
			TRUN_AUDIO_FLAGS);
	}

	return p;
}

////// Muxer

static void
mp4_muxer_init_track(
	mp4_muxer_state_t* state,
	mp4_muxer_stream_state_t* cur_stream,
	media_track_t* cur_track)
{
	cur_stream->timescale = cur_track->media_info.timescale;
	cur_stream->media_type = cur_track->media_info.media_type;
	cur_stream->first_frame_part = &cur_track->frames;
	cur_stream->cur_frame_part = cur_track->frames;
	cur_stream->cur_frame = cur_track->frames.first_frame;
	cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);

	cur_stream->first_frame_time_offset = 
		mp4_rescale_millis(cur_track->clip_start_time, cur_track->media_info.timescale) + 
		cur_track->first_frame_time_offset;
	cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;

	if (!state->reuse_buffers)
	{
		cur_stream->cur_frame_part.frames_source->disable_buffer_reuse(
			cur_stream->cur_frame_part.frames_source_context);
	}
}

static void
mp4_muxer_reinit_tracks(mp4_muxer_state_t* state)
{
	media_track_t* cur_track;
	mp4_muxer_stream_state_t* cur_stream;

	state->first_time = TRUE;

	cur_track = state->first_clip_track;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_track++)
	{
		mp4_muxer_init_track(state, cur_stream, cur_track);
	}
	state->first_clip_track = cur_track;
}

static vod_status_t
mp4_muxer_choose_stream(mp4_muxer_state_t* state)
{
	mp4_muxer_stream_state_t* cur_stream;
	mp4_muxer_stream_state_t* min_dts = NULL;
	uint64_t min_time_offset = 0;

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

			if (min_dts == NULL || 
				cur_stream->next_frame_time_offset < min_time_offset)
			{
				min_dts = cur_stream;

				min_time_offset = min_dts->next_frame_time_offset;
				if (min_dts != state->selected_stream)
				{
					min_time_offset += min_dts->timescale / 4;		// prefer the last selected stream, allow 0.25s delay
				}
			}
		}

		if (min_dts != NULL)
		{
			state->selected_stream = min_dts;
			return VOD_OK;
		}

		if (state->first_clip_track >= state->media_set->filtered_tracks_end)
		{
			break;
		}

		mp4_muxer_reinit_tracks(state);
	}

	return VOD_NOT_FOUND;
}

static vod_status_t
mp4_calculate_output_offsets(
	mp4_muxer_state_t* state, 
	size_t* frames_size,
	uint32_t* trun_atom_count)
{
	mp4_muxer_stream_state_t* selected_stream;
	mp4_muxer_stream_state_t* cur_stream;
	uint32_t cur_offset = 0;
	vod_status_t rc;

	*trun_atom_count = 0;

	for (;;)
	{
		// choose a stream
		rc = mp4_muxer_choose_stream(state);
		if (rc != VOD_OK)
		{
			if (rc == VOD_NOT_FOUND)
			{
				break;		// done
			}
			return rc;
		}

		selected_stream = state->selected_stream;

		// check for a stream switch
		if (selected_stream->last_frame_size == UINT_MAX ||
			cur_offset != selected_stream->cur_frame_output_offset[-1] + selected_stream->last_frame_size)
		{
			(*trun_atom_count)++;
		}
		selected_stream->last_frame_size = selected_stream->cur_frame->size;

		// set the offset (points to the beginning of the actual data)
		*selected_stream->cur_frame_output_offset = cur_offset;
		selected_stream->cur_frame_output_offset++;

		// update the offset
		cur_offset += selected_stream->last_frame_size;

		// move to the next frame
		selected_stream->next_frame_time_offset += selected_stream->cur_frame->duration;
		selected_stream->cur_frame++;
	}

	// reset the state
	if (state->media_set->clip_count > 1)
	{
		state->first_clip_track = state->media_set->filtered_tracks;
		mp4_muxer_reinit_tracks(state);

		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;
		}
	}
	else
	{
		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			cur_stream->cur_frame_part = *cur_stream->first_frame_part;
			cur_stream->cur_frame = cur_stream->cur_frame_part.first_frame;
			cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);
			cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;
			cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
		}
	}
	state->selected_stream = NULL;

	*frames_size = cur_offset;

	return VOD_OK;
}

static vod_status_t
mp4_muxer_init_state(
	request_context_t* request_context,
	media_set_t* media_set,
	segment_writer_t* track_writers,
	bool_t per_stream_writer,
	bool_t reuse_buffers,
	mp4_muxer_state_t** result)
{
	media_track_t* cur_track;
	mp4_muxer_stream_state_t* cur_stream;
	mp4_muxer_state_t* state;
	uint32_t clip_index;
	uint32_t index;

	// allocate the state and stream states
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_muxer_init_state: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->first_stream = vod_alloc(
		request_context->pool, 
		sizeof(state->first_stream[0]) * media_set->total_track_count);
	if (state->first_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_muxer_init_state: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	state->last_stream = state->first_stream + media_set->total_track_count;
	state->request_context = request_context;
	state->reuse_buffers = reuse_buffers;
	state->media_set = media_set;
	state->per_stream_writer = per_stream_writer;
	state->cur_frame = NULL;
	state->selected_stream = NULL;
	state->first_time = TRUE;

	index = 0;
	cur_track = media_set->filtered_tracks;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_track++, index++)
	{
		cur_stream->index = index;
		cur_stream->write_callback = track_writers->write_tail;
		cur_stream->write_context = track_writers->context;
		if (per_stream_writer)
		{
			track_writers++;
		}

		// get total frame count for this stream
		cur_stream->frame_count = cur_track->frame_count;
		for (clip_index = 1; clip_index < media_set->clip_count; clip_index++)
		{
			cur_stream->frame_count += cur_track[clip_index * media_set->total_track_count].frame_count;
		}

		// allocate the output offset
		cur_stream->first_frame_output_offset = vod_alloc(
			request_context->pool,
			cur_stream->frame_count * sizeof(cur_stream->first_frame_output_offset[0]));
		if (cur_stream->first_frame_output_offset == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_muxer_init_state: vod_alloc failed (3)");
			return VOD_ALLOC_FAILED;
		}
		cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;
		cur_stream->last_frame_size = UINT_MAX;

		// init the stream
		mp4_muxer_init_track(state, cur_stream, cur_track);
	}

	state->first_clip_track = cur_track;

	*result = state;

	return VOD_OK;
}

static uint64_t
mp4_muxer_get_earliest_pres_time(media_set_t* media_set, uint32_t index)
{
	media_track_t* track;
	uint64_t result = 0;
	uint32_t clip_index;

	for (clip_index = 0, track = media_set->filtered_tracks + index;
		clip_index < media_set->clip_count; 
		clip_index++, track += media_set->total_track_count)
	{
		result = mp4_rescale_millis(track->clip_start_time, track->media_info.timescale) +
			track->first_frame_time_offset;

		if (track->frame_count <= 0)
		{
			continue;
		}

		if (track->media_info.media_type == MEDIA_TYPE_VIDEO)
		{
			result += track->frames.first_frame[0].pts_delay;
			result -= track->media_info.u.video.initial_pts_delay;
		}

		break;
	}

	return result;
}

vod_status_t
mp4_muxer_init_fragment(
	request_context_t* request_context,
	uint32_t segment_index,
	media_set_t* media_set,
	segment_writer_t* track_writers,
	bool_t per_stream_writer,
	bool_t reuse_buffers,
	bool_t size_only,
	vod_str_t* header, 
	size_t* total_fragment_size,
	mp4_muxer_state_t** processor_state)
{
	mp4_muxer_stream_state_t* cur_stream;
	mp4_muxer_state_t* state;
	vod_status_t rc;
	uint64_t earliest_pres_time;
	uint32_t trun_atom_count;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t mdat_atom_size = 0;
	size_t result_size;
	u_char* traf_header;
	u_char* p;

	// initialize the muxer state
	rc = mp4_muxer_init_state(
		request_context, 
		media_set,
		track_writers, 
		per_stream_writer,
		reuse_buffers,
		&state);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_muxer_init_fragment: mp4_muxer_init_state failed %i", rc);
		return rc;
	}

	// init output offsets and get the mdat size
	rc = mp4_calculate_output_offsets(state, &mdat_atom_size, &trun_atom_count);
	if (rc != VOD_OK)
	{
		return rc;
	}
	mdat_atom_size += MDAT_HEADER_SIZE;

	// get the moof size
	moof_atom_size =
		ATOM_HEADER_SIZE +		// moof
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t) + 
		(ATOM_HEADER_SIZE +		// traf
		ATOM_HEADER_SIZE + sizeof(tfhd_atom_t) +
		ATOM_HEADER_SIZE + sizeof(tfdt64_atom_t)) * media_set->total_track_count + 
		(ATOM_HEADER_SIZE + sizeof(trun_atom_t)) * trun_atom_count;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		switch (cur_stream->media_type)
		{
		case MEDIA_TYPE_VIDEO:
			moof_atom_size += cur_stream->frame_count * sizeof(trun_video_frame_t);
			break;
		case MEDIA_TYPE_AUDIO:
			moof_atom_size += cur_stream->frame_count * sizeof(trun_audio_frame_t);
			break;
		}
	}
	
	*total_fragment_size =
		moof_atom_size +
		mdat_atom_size;

	// head request optimization
	if (size_only)
	{
		return VOD_OK;
	}

	// allocate the response
	result_size =
		moof_atom_size +
		MDAT_HEADER_SIZE;

	header->data = vod_alloc(request_context->pool, result_size);
	if (header->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_muxer_init_fragment: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	p = header->data;

	// moof
	write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

	// moof.mfhd
	p = mp4_fragment_write_mfhd_atom(p, segment_index);

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		// skip moof.traf
		traf_header = p;
		p += ATOM_HEADER_SIZE;

		// moof.traf.tfhd
		p = mp4_fragment_write_tfhd_atom(p, cur_stream->index + 1, 0);

		// moof.traf.tfdt
		earliest_pres_time = mp4_muxer_get_earliest_pres_time(
			media_set, 
			cur_stream->index);
		p = mp4_fragment_write_tfdt64_atom(p, earliest_pres_time);

		// moof.traf.trun
		switch (cur_stream->media_type)
		{
		case MEDIA_TYPE_VIDEO:
			p = mp4_muxer_write_video_trun_atoms(
				p,
				media_set,
				cur_stream,
				moof_atom_size + MDAT_HEADER_SIZE);
			break;

		case MEDIA_TYPE_AUDIO:
			p = mp4_muxer_write_audio_trun_atoms(
				p,
				media_set,
				cur_stream,
				moof_atom_size + MDAT_HEADER_SIZE);
			break;
		}

		// moof.traf
		traf_atom_size = p - traf_header;
		write_atom_header(traf_header, traf_atom_size, 't', 'r', 'a', 'f');
	}

	// mdat
	write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');

	header->len = p - header->data;

	if (header->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_muxer_init_fragment: result length %uz exceeded allocated length %uz",
			header->len, result_size);
		return VOD_UNEXPECTED;
	}

	rc = mp4_muxer_start_frame(state);
	if (rc != VOD_OK)
	{
		if (rc == VOD_NOT_FOUND)
		{
			*processor_state = NULL;		// no frames, nothing to do
			return VOD_OK;
		}

		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_muxer_init_fragment: mp4_muxer_start_frame failed %i", rc);
		return rc;
	}

	*processor_state = state;
	return VOD_OK;
}

static vod_status_t
mp4_muxer_start_frame(mp4_muxer_state_t* state)
{
	mp4_muxer_stream_state_t* selected_stream;
	mp4_muxer_stream_state_t* cur_stream;
	read_cache_hint_t cache_hint;
	input_frame_t* cur_frame;
	vod_status_t rc;

	rc = mp4_muxer_choose_stream(state);
	if (rc != VOD_OK)
	{
		return rc;
	}
	selected_stream = state->selected_stream;

	// init the frame
	state->cur_frame = selected_stream->cur_frame;
	state->frames_source = selected_stream->cur_frame_part.frames_source;
	state->frames_source_context = selected_stream->cur_frame_part.frames_source_context;
	selected_stream->cur_frame++;
	selected_stream->cur_frame_output_offset++;

	selected_stream->next_frame_time_offset += state->cur_frame->duration;

	state->cache_slot_id = selected_stream->media_type;

	// find the min offset
	cache_hint.min_offset = ULLONG_MAX;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream)
		{
			continue;
		}

		cur_frame = cur_stream->cur_frame;
		if (cur_frame < cur_stream->cur_frame_part.last_frame &&
			cur_frame->offset < cache_hint.min_offset &&
			cur_stream->source == selected_stream->source)
		{
			cache_hint.min_offset = cur_frame->offset;
			cache_hint.min_offset_slot_id = cur_stream->media_type;
		}
	}

	rc = state->frames_source->start_frame(state->frames_source_context, state->cur_frame, &cache_hint);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

vod_status_t
mp4_muxer_process_frames(mp4_muxer_state_t* state)
{
	mp4_muxer_stream_state_t* selected_stream = state->selected_stream;
	mp4_muxer_stream_state_t* last_stream = NULL;
	u_char* read_buffer;
	uint32_t read_size;
	u_char* write_buffer = NULL;
	uint32_t write_buffer_size = 0;
	vod_status_t rc;
	bool_t processed_data = FALSE;
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

			if (write_buffer_size != 0)
			{
				// flush the write buffer
				rc = last_stream->write_callback(last_stream->write_context, write_buffer, write_buffer_size);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			else if (!processed_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_muxer_process_frames: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}

		processed_data = TRUE;

		if (state->reuse_buffers)
		{
			rc = selected_stream->write_callback(selected_stream->write_context, read_buffer, read_size);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		else if (write_buffer_size != 0)
		{
			// if the buffers are contiguous, just increment the size
			if (write_buffer + write_buffer_size == read_buffer &&
				(last_stream == selected_stream || !state->per_stream_writer))
			{
				write_buffer_size += read_size;
			}
			else
			{
				// buffers not contiguous, flush the write buffer
				rc = last_stream->write_callback(last_stream->write_context, write_buffer, write_buffer_size);
				if (rc != VOD_OK)
				{
					return rc;
				}

				// reset the write buffer
				write_buffer = read_buffer;
				write_buffer_size = read_size;
				last_stream = selected_stream;
			}
		}
		else
		{
			// reset the write buffer
			write_buffer = read_buffer;
			write_buffer_size = read_size;
			last_stream = selected_stream;
		}

		if (!frame_done)
		{
			continue;
		}

		if (selected_stream->cur_frame >= selected_stream->cur_frame_part.last_frame)
		{
			if (write_buffer_size != 0)
			{
				// flush the write buffer
				rc = last_stream->write_callback(last_stream->write_context, write_buffer, write_buffer_size);
				if (rc != VOD_OK)
				{
					return rc;
				}

				write_buffer_size = 0;
			}
		}

		// start a new frame
		rc = mp4_muxer_start_frame(state);
		if (rc != VOD_OK)
		{
			if (rc == VOD_NOT_FOUND)
			{
				break;		// done
			}

			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mp4_muxer_process_frames: mp4_muxer_start_frame failed %i", rc);
			return rc;
		}

		selected_stream = state->selected_stream;
	}

	return VOD_OK;
}
