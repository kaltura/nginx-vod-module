#include "filter.h"
#include "audio_filter.h"
#include "rate_filter.h"
#include "concat_clip.h"
#include "../media_set.h"
#include "../segmenter.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	vod_uint_t manifest_duration_policy;
	media_sequence_t* sequence;
	media_clip_filtered_t* output_clip;
	media_track_t* output_track;
	media_track_t* audio_reference_track;
	uint32_t audio_reference_track_speed_num;
	uint32_t audio_reference_track_speed_denom;
	bool_t has_audio_frames;
	uint32_t source_count;
} filters_init_state_t;

typedef struct {
	request_context_t* request_context;
	read_cache_state_t* read_cache_state;
	media_set_t* media_set;
	media_sequence_t* sequence;
	media_clip_filtered_t* output_clip;
	media_track_t* cur_track;
	void* audio_filter;
	uint32_t max_frame_count;
	uint32_t output_codec_id;
} apply_filters_state_t;

static void
filter_get_clip_track_count(media_clip_t* clip, uint32_t* track_count)
{
	media_clip_source_t* source;
	media_track_t* cur_track;
	media_clip_t** cur_source;
	media_clip_t** sources_end;

	if (media_clip_is_source(clip->type))
	{
		source = vod_container_of(clip, media_clip_source_t, base);
		for (cur_track = source->track_array.first_track;
			cur_track < source->track_array.last_track;
			cur_track++)
		{
			track_count[cur_track->media_info.media_type]++;
		}
		return;
	}

	// recursively count child sources
	if (clip->type == MEDIA_CLIP_CONCAT)
	{
		sources_end = clip->sources + 1;
	}
	else
	{
		sources_end = clip->sources + clip->source_count;
	}

	for (cur_source = clip->sources; cur_source < sources_end; cur_source++)
	{
		filter_get_clip_track_count(*cur_source, track_count);
	}
}

static media_track_t*
filter_copy_track_to_clip(
	filters_init_state_t* state, 
	media_track_t* track)
{
	media_track_t* output_track = state->output_track;
	media_track_t** ref_track = state->output_clip->ref_track;
	uint32_t media_type;

	*output_track = *track;

	media_type = output_track->media_info.media_type;
	if (ref_track[media_type] == NULL)
	{ 
		ref_track[media_type] = output_track;
	}
	else
	{
		switch (state->manifest_duration_policy)
		{
		case MDP_MAX:
			if (output_track->media_info.duration_millis > ref_track[media_type]->media_info.duration_millis)
			{
				ref_track[media_type] = output_track;
			}
			break;

		case MDP_MIN:
			if (output_track->media_info.duration_millis > 0 && 
				(ref_track[media_type]->media_info.duration_millis == 0 ||
				output_track->media_info.duration_millis < ref_track[media_type]->media_info.duration_millis))
			{
				ref_track[media_type] = output_track;
			}
			break;
		}
	}

	if (output_track->media_info.media_type == MEDIA_TYPE_VIDEO)
	{
		state->sequence->video_key_frame_count += output_track->key_frame_count;
	}

	state->sequence->total_frame_count += output_track->frame_count;
	state->sequence->total_frame_size += output_track->total_frames_size;

	state->output_track++;

	return output_track;
}

static void
filter_init_filtered_clip_from_source(
	filters_init_state_t* state, 
	media_clip_source_t* source)
{
	uint32_t media_type;
	media_track_t* cur_track;

	// copy tracks in media type order (video first then audio)
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		for (cur_track = source->track_array.first_track; cur_track < source->track_array.last_track; cur_track++)
		{
			if (cur_track->media_info.media_type == media_type)
			{
				filter_copy_track_to_clip(state, cur_track);
			}
		}
	}
}

static vod_status_t
filter_scale_video_tracks(filters_init_state_t* state, media_clip_t* clip, uint32_t speed_num, uint32_t speed_denom)
{
	media_clip_rate_filter_t* rate_filter;
	media_clip_source_t* source;
	media_track_t* new_track;
	media_track_t* cur_track;
	media_clip_t** cur_source;
	media_clip_t** sources_end;
	vod_status_t rc;

	if (media_clip_is_source(clip->type))
	{
		source = vod_container_of(clip, media_clip_source_t, base);

		// reset the sequence pointer, may have shifted in case one more sequences were removed
		source->sequence = state->sequence;

		for (cur_track = source->track_array.first_track;
			cur_track < source->track_array.last_track;
			cur_track++)
		{
			switch (cur_track->media_info.media_type)
			{
			case MEDIA_TYPE_AUDIO:
				if (state->audio_reference_track == NULL)
				{
					state->audio_reference_track = cur_track;
					state->audio_reference_track_speed_num = speed_num;
					state->audio_reference_track_speed_denom = speed_denom;
				}
				if (cur_track->frame_count > 0)
				{
					state->has_audio_frames = TRUE;
				}
				break;

			default:
				new_track = filter_copy_track_to_clip(state, cur_track);
				if (speed_num != speed_denom)
				{
					rate_filter_scale_track_timestamps(new_track, speed_num, speed_denom);
				}
				break;
			}
		}

		state->source_count++;
		return VOD_OK;
	}

	// recursively filter sources
	switch (clip->type)
	{
	case MEDIA_CLIP_RATE_FILTER:
		rate_filter = vod_container_of(clip, media_clip_rate_filter_t, base);
		speed_num = ((uint64_t)speed_num * rate_filter->rate.num) / rate_filter->rate.denom;
		break;

	default:;
	}

	if (clip->type == MEDIA_CLIP_CONCAT && clip->source_count > 1)
	{
		rc = concat_clip_concat(state->request_context, clip);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	sources_end = clip->sources + clip->source_count;
	for (cur_source = clip->sources; cur_source < sources_end; cur_source++)
	{
		rc = filter_scale_video_tracks(state, *cur_source, speed_num, speed_denom);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t
filter_validate_consistent_codecs(
	request_context_t* request_context,
	media_clip_filtered_t* first_clip, 
	media_clip_filtered_t* cur_clip)
{
	media_track_t* first_clip_track;
	media_track_t* cur_clip_track;

	for (first_clip_track = first_clip->first_track, cur_clip_track = cur_clip->first_track;
		first_clip_track < first_clip->last_track;
		first_clip_track++, cur_clip_track++)
	{
		if (first_clip_track->media_info.codec_id != cur_clip_track->media_info.codec_id)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"filter_validate_consistent_codecs: track codec changed, current=%uD initial=%uD",
				cur_clip_track->media_info.codec_id, first_clip_track->media_info.codec_id);
			return VOD_BAD_REQUEST;
		}
	}

	return VOD_OK;
}

vod_status_t
filter_init_filtered_clips(
	request_context_t* request_context,
	media_set_t* media_set,
	bool_t parsed_frames)
{
	filters_init_state_t init_state;
	media_clip_filtered_t* output_clip;
	media_sequence_t* sequence;
	media_clip_t** clips_end;
	media_clip_t** cur_clip;
	media_clip_t* input_clip;
	media_track_t* new_track;
	uint32_t track_count[MEDIA_TYPE_COUNT];
	uint32_t clip_index;
	uint32_t media_type;
	uint32_t cur_count;
	vod_status_t rc;

	media_set->audio_filtering_needed = FALSE;
	vod_memzero(media_set->track_count, sizeof(media_set->track_count));
	media_set->total_track_count = 0;

	// allocate the filtered clips
	output_clip = vod_alloc(
		request_context->pool,
		sizeof(output_clip[0]) * media_set->sequence_count * media_set->clip_count);
	if (output_clip == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"filter_init_filtered_clips: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	for (sequence = media_set->sequences;
		sequence < media_set->sequences_end;
		sequence++)
	{
		// init sequence counters
		sequence->total_frame_size = 0;
		sequence->video_key_frame_count = 0;
		sequence->total_frame_count = 0;

		// get max number of tracks in the clips of the sequence
		vod_memzero(sequence->track_count, sizeof(sequence->track_count));

		clips_end = sequence->clips + media_set->clip_count;
		for (cur_clip = sequence->clips; cur_clip < clips_end; cur_clip++)
		{
			vod_memzero(track_count, sizeof(track_count));
			filter_get_clip_track_count(*cur_clip, track_count);

			if (!media_clip_is_source(cur_clip[0]->type) && track_count[MEDIA_TYPE_AUDIO] > 1)
			{
				track_count[MEDIA_TYPE_AUDIO] = 1;		// audio filtering supports only a single output track
			}

			if (cur_clip == sequence->clips)
			{
				vod_memcpy(sequence->track_count, track_count, sizeof(sequence->track_count));
				continue;
			}

			for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
			{
				if (sequence->track_count[media_type] != track_count[media_type])
				{
					vod_log_error(VOD_LOG_ERR, request_context->log, 0,
						"filter_init_filtered_clips: track count mismatch, first clip had %uD current clip has %uD media type %uD",
						sequence->track_count[media_type], track_count[media_type], media_type);
					return VOD_BAD_MAPPING;
				}
			}
		}

		// update track counts
		sequence->total_track_count = 0;
		for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
		{
			cur_count = sequence->track_count[media_type];
			if (cur_count <= 0)
			{
				continue;
			}

			sequence->total_track_count += cur_count;
			media_set->track_count[media_type] += cur_count;
			sequence->media_type = media_type;
		}
		media_set->total_track_count += sequence->total_track_count;

		switch (sequence->total_track_count)
		{
		case 0:
			media_set->sequence_count--;
			media_set->sequences_end--;
			vod_memmove(sequence, sequence + 1, (u_char*)media_set->sequences_end - (u_char*)sequence);
			sequence--;
			continue;

		case 1:
			// sequence->media_type already set
			break;

		default:
			sequence->media_type = MEDIA_TYPE_NONE;
			break;
		}

		// initialize the filtered clips array
		sequence->filtered_clips = output_clip;
		output_clip += media_set->clip_count;
		sequence->filtered_clips_end = output_clip;
	}

	// allocate the output tracks
	init_state.output_track = vod_alloc(
		request_context->pool,
		sizeof(*init_state.output_track) * media_set->total_track_count * media_set->clip_count);
	if (init_state.output_track == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"filter_init_filtered_clips: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	init_state.request_context = request_context;
	init_state.manifest_duration_policy = media_set->segmenter_conf->manifest_duration_policy;

	media_set->filtered_tracks = init_state.output_track;

	for (clip_index = 0; clip_index < media_set->clip_count; clip_index++)
	{
		for (sequence = media_set->sequences;
			sequence < media_set->sequences_end;
			sequence++)
		{
			input_clip = sequence->clips[clip_index];
			output_clip = &sequence->filtered_clips[clip_index];

			output_clip->first_track = init_state.output_track;

			vod_memzero(output_clip->ref_track, sizeof(output_clip->ref_track));

			// initialize the state
			init_state.sequence = sequence;
			init_state.output_clip = output_clip;
			init_state.audio_reference_track = NULL;

			// in case of source, just copy all tracks as is
			if (media_clip_is_source(input_clip->type))
			{
				filter_init_filtered_clip_from_source(&init_state, (media_clip_source_t*)input_clip);

				// reset the sequence pointer, may have shifted in case one more sequences were removed
				((media_clip_source_t*)input_clip)->sequence = sequence;
			}
			else
			{
				init_state.has_audio_frames = FALSE;
				init_state.source_count = 0;

				rc = filter_scale_video_tracks(&init_state, input_clip, 100, 100);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

			if (init_state.audio_reference_track != NULL)
			{
				// add the audio filter output track
				new_track = filter_copy_track_to_clip(&init_state, init_state.audio_reference_track);
				if (init_state.audio_reference_track_speed_num != init_state.audio_reference_track_speed_denom)
				{
					rate_filter_scale_track_timestamps(
						new_track,
						init_state.audio_reference_track_speed_num,
						init_state.audio_reference_track_speed_denom);
				}

				if (!parsed_frames || init_state.has_audio_frames)
				{
					new_track->source_clip = input_clip;
					media_set->audio_filtering_needed = TRUE;
				}
			}

			output_clip->last_track = init_state.output_track;

			// make sure all clips have the same codecs
			if (clip_index > 0)
			{
				rc = filter_validate_consistent_codecs(
					request_context,
					sequence->filtered_clips,
					output_clip);
				if (rc != VOD_OK)
				{
					return rc;
				}

				continue;
			}
		}
	}

	media_set->filtered_tracks_end = init_state.output_track;

	if (media_set->timing.durations == NULL)
	{
		media_set->timing.total_duration = segmenter_get_total_duration(
				media_set->segmenter_conf,
				media_set,
				media_set->sequences,
				media_set->sequences_end,
				MEDIA_TYPE_NONE);
	}

	return VOD_OK;
}

vod_status_t 
filter_init_state(
	request_context_t* request_context,
	read_cache_state_t* read_cache_state,
	media_set_t* media_set,
	uint32_t max_frame_count,
	uint32_t output_codec_id,
	void** context)
{
	apply_filters_state_t* state;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"filter_init_state: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
		
	state->request_context = request_context;
	state->read_cache_state = read_cache_state;
	state->media_set = media_set;
	state->sequence = media_set->sequences;
	state->output_clip = state->sequence->filtered_clips;
	state->cur_track = state->output_clip->first_track;
	state->max_frame_count = max_frame_count;
	state->output_codec_id = output_codec_id;
	state->audio_filter = NULL;

	*context = state;

	return VOD_OK;
}

vod_status_t
filter_run_state_machine(void* context)
{
	apply_filters_state_t* state = context;
	vod_status_t rc;
	size_t cache_buffer_count;

	for (;;)
	{
		if (state->audio_filter != NULL)
		{
			// run the audio filter
			rc = audio_filter_process(state->audio_filter);
			if (rc != VOD_OK)
			{
				return rc;
			}

			audio_filter_free_state(state->audio_filter);
			state->audio_filter = NULL;

			state->cur_track++;
		}
		
		if (state->cur_track >= state->output_clip->last_track)
		{
			// move to the next track
			state->output_clip++;

			if (state->output_clip >= state->sequence->filtered_clips_end)
			{
				state->sequence++;
				if (state->sequence >= state->media_set->sequences_end)
				{
					return VOD_OK;
				}

				state->output_clip = state->sequence->filtered_clips;
			}

			state->cur_track = state->output_clip->first_track;
		}

		if (state->cur_track->source_clip == NULL)
		{
			state->cur_track++;
			continue;
		}

		// initialize the audio filter
		rc = audio_filter_alloc_state(
			state->request_context,
			state->sequence,
			state->cur_track->source_clip,
			state->cur_track,
			state->max_frame_count,
			state->output_codec_id,
			&cache_buffer_count,
			&state->audio_filter);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (state->audio_filter == NULL)
		{
			state->cur_track++;
		}
		else
		{
			// make sure the cache has enough slots
			rc = read_cache_allocate_buffer_slots(state->read_cache_state, cache_buffer_count);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}
}
