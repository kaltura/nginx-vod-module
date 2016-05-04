#include "manifest_utils.h"

// typedefs
typedef struct {
	vod_str_t label;
	uint32_t track_count;
} label_track_count_t;

static u_char*
manifest_utils_write_bitmask(u_char* p, uint32_t bitmask, u_char letter)
{
	uint32_t i;

	for (i = 0; i < 32; i++)
	{
		if ((bitmask & (1 << i)) == 0)
		{
			continue;
		}

		*p++ = '-';
		*p++ = letter;
		p = vod_sprintf(p, "%uD", i + 1);
	}

	return p;
}

static vod_status_t
manifest_utils_build_request_params_string_per_sequence_tracks(
	request_context_t* request_context, 
	uint32_t segment_index,
	uint32_t sequences_mask,
	uint32_t* sequence_tracks_mask,
	vod_str_t* result)
{
	u_char* p;
	size_t result_size;
	uint32_t* tracks_mask;
	uint32_t i;

	result_size = 0;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		result_size += 1 + vod_get_int_print_len(segment_index + 1);
	}

	for (i = 0, tracks_mask = sequence_tracks_mask;
		i < MAX_SEQUENCES;
		i++, tracks_mask += MEDIA_TYPE_COUNT)
	{
		if ((sequences_mask & (1 << i)) == 0)
		{
			continue;
		}

		// sequence
		result_size += sizeof("-f32") - 1;

		// video tracks
		if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			result_size += sizeof("-v0") - 1;
		}
		else
		{
			result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_VIDEO]) * (sizeof("-v32") - 1);
		}

		// audio tracks
		if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
		{
			result_size += sizeof("-a0") - 1;
		}
		else
		{
			result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_AUDIO]) * (sizeof("-a32") - 1);
		}
	}

	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_build_request_params_string_per_sequence_tracks: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		p = vod_sprintf(p, "-%uD", segment_index + 1);
	}

	for (i = 0, tracks_mask = sequence_tracks_mask; 
		i < MAX_SEQUENCES;
		i++, tracks_mask += MEDIA_TYPE_COUNT)
	{
		if ((sequences_mask & (1 << i)) == 0)
		{
			continue;
		}

		// sequence
		p = vod_sprintf(p, "-f%uD", i + 1);

		// video tracks
		switch (tracks_mask[MEDIA_TYPE_VIDEO])
		{
		case 0xffffffff:
			p = vod_copy(p, "-v0", sizeof("-v0") - 1);
			break;

		case 0:
			break;

		default:
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_VIDEO], 'v');
			break;
		}

		// audio tracks
		switch (tracks_mask[MEDIA_TYPE_AUDIO])
		{
		case 0xffffffff:
			p = vod_copy(p, "-a0", sizeof("-a0") - 1);
			break;

		case 0:
			break;

		default:
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_AUDIO], 'a');
			break;
		}
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"manifest_utils_build_request_params_string_per_sequence_tracks: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
manifest_utils_build_request_params_string(
	request_context_t* request_context, 
	uint32_t* has_tracks,
	uint32_t segment_index,
	uint32_t sequences_mask,
	uint32_t* sequence_tracks_mask,
	uint32_t* tracks_mask,
	vod_str_t* result)
{
	u_char* p;
	size_t result_size;

	if (sequence_tracks_mask != NULL)
	{
		return manifest_utils_build_request_params_string_per_sequence_tracks(
			request_context,
			segment_index,
			sequences_mask,
			sequence_tracks_mask,
			result);
	}

	result_size = 0;
	
	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		result_size += 1 + vod_get_int_print_len(segment_index + 1);
	}
	
	// sequence mask
	if (sequences_mask != 0xffffffff)
	{
		result_size += vod_get_number_of_set_bits(sequences_mask) * (sizeof("-f32") - 1);
	}

	// video tracks
	if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
	{
		result_size += sizeof("-v0") - 1;
	}
	else
	{
		result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_VIDEO]) * (sizeof("-v32") - 1);
	}
	
	// audio tracks
	if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
	{
		result_size += sizeof("-a0") - 1;
	}
	else
	{
		result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_AUDIO]) * (sizeof("-a32") - 1);
	}
	
	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_build_request_params_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		p = vod_sprintf(p, "-%uD", segment_index + 1);
	}
	
	// sequence mask
	if (sequences_mask != 0xffffffff)
	{
		p = manifest_utils_write_bitmask(p, sequences_mask, 'f');
	}

	// video tracks
	if (has_tracks[MEDIA_TYPE_VIDEO])
	{
		if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			p = vod_copy(p, "-v0", sizeof("-v0") - 1);
		}
		else
		{
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_VIDEO], 'v');
		}
	}
	
	// audio tracks
	if (has_tracks[MEDIA_TYPE_AUDIO])
	{
		if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
		{
			p = vod_copy(p, "-a0", sizeof("-a0") - 1);
		}
		else
		{
			p = manifest_utils_write_bitmask(p, tracks_mask[MEDIA_TYPE_AUDIO], 'a');
		}
	}
	
	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"manifest_utils_build_request_params_string: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

u_char*
manifest_utils_append_tracks_spec(u_char* p, media_track_t** tracks, bool_t write_sequence_index)
{
	media_sequence_t* video_sequence = NULL;
	media_sequence_t* audio_sequence;

	if (tracks[MEDIA_TYPE_VIDEO] != NULL)
	{
		if (write_sequence_index)
		{
			video_sequence = tracks[MEDIA_TYPE_VIDEO]->file_info.source->sequence;
			p = vod_sprintf(p, "-f%uD", video_sequence->index + 1);
		}
		p = vod_sprintf(p, "-v%uD", tracks[MEDIA_TYPE_VIDEO]->index + 1);
	}

	if (tracks[MEDIA_TYPE_AUDIO] != NULL)
	{
		if (write_sequence_index)
		{
			audio_sequence = tracks[MEDIA_TYPE_AUDIO]->file_info.source->sequence;
			if (video_sequence == NULL || audio_sequence->index != video_sequence->index)
			{
				p = vod_sprintf(p, "-f%uD", audio_sequence->index + 1);
			}
		}
		p = vod_sprintf(p, "-a%uD", tracks[MEDIA_TYPE_AUDIO]->index + 1);
	}

	return p;
}

static vod_status_t
manifest_utils_get_unique_labels(
	request_context_t* request_context,
	media_set_t* media_set, 
	label_track_count_t** output,
	size_t* output_count)
{
	vod_str_t* cur_track_label;
	label_track_count_t* first_label;
	label_track_count_t* last_label;
	label_track_count_t* cur_label;
	media_track_t* last_track;
	media_track_t* cur_track;
	bool_t label_found;

	first_label = vod_alloc(request_context->pool, 
		media_set->total_track_count * sizeof(first_label[0]));
	if (first_label == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_get_unique_labels: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	last_label = first_label;

	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
	{
		if (cur_track->media_info.media_type != MEDIA_TYPE_AUDIO ||
			cur_track->media_info.label.len == 0)
		{
			continue;
		}

		cur_track_label = &cur_track->media_info.label;

		label_found = FALSE;
		for (cur_label = first_label; cur_label < last_label; cur_label++)
		{
			if (vod_str_equals(*cur_track_label, cur_label->label))
			{
				label_found = TRUE;
				break;
			}
		}

		if (label_found)
		{
			cur_label->track_count++;
			continue;
		}

		last_label->label = *cur_track_label;
		last_label->track_count = 1;
		last_label++;
	}

	*output = first_label;
	*output_count = last_label - first_label;

	return VOD_OK;
}

static vod_status_t
manifest_utils_get_muxed_adaptation_set(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	vod_str_t* label,
	adaptation_set_t* output)
{
	media_sequence_t* cur_sequence;
	media_track_t** cur_track_ptr;
	media_track_t* audio_track;
	media_track_t* last_track;
	media_track_t* cur_track;
	uint32_t main_media_type;

	// allocate the tracks array
	cur_track_ptr = vod_alloc(request_context->pool, 
		sizeof(output->first[0]) * media_set->total_track_count * MEDIA_TYPE_COUNT);
	if (cur_track_ptr == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_get_muxed_adaptation_set: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	output->type = ADAPTATION_TYPE_MUXED;
	output->first = cur_track_ptr;
	output->count = 0;

	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		// find the main media type
		if (cur_sequence->track_count[MEDIA_TYPE_VIDEO] > 0)
		{
			main_media_type = MEDIA_TYPE_VIDEO;
		}
		else
		{
			if ((flags & ADAPTATION_SETS_FLAG_AVOID_AUDIO_ONLY) != 0 && 
				media_set->track_count[MEDIA_TYPE_VIDEO] > 0)
			{
				continue;
			}

			main_media_type = MEDIA_TYPE_AUDIO;
		}

		// find the audio track
		audio_track = cur_sequence->filtered_clips[0].longest_track[MEDIA_TYPE_AUDIO];
		if ((audio_track == NULL || (label != NULL && !vod_str_equals(*label, audio_track->media_info.label))) &&
			media_set->track_count[MEDIA_TYPE_AUDIO] > 0)
		{
			if (cur_sequence->track_count[MEDIA_TYPE_VIDEO] <= 0)
			{
				continue;
			}

			// find some audio track from another sequence to mux with this video
			last_track = media_set->filtered_tracks + media_set->total_track_count;
			for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type == MEDIA_TYPE_AUDIO && 
					(label == NULL || vod_str_equals(*label, cur_track->media_info.label)))
				{
					audio_track = cur_track;
					break;
				}
			}
		}

		for (cur_track = cur_sequence->filtered_clips[0].first_track; cur_track < cur_sequence->filtered_clips[0].last_track; cur_track++)
		{
			if (cur_track->media_info.media_type != main_media_type)
			{
				continue;
			}

			// add the track
			if (main_media_type == MEDIA_TYPE_VIDEO)
			{
				cur_track_ptr[MEDIA_TYPE_VIDEO] = cur_track;
				cur_track_ptr[MEDIA_TYPE_AUDIO] = audio_track;
			}
			else
			{
				cur_track_ptr[MEDIA_TYPE_VIDEO] = NULL;
				cur_track_ptr[MEDIA_TYPE_AUDIO] = cur_track;
			}

			cur_track_ptr += MEDIA_TYPE_COUNT;

			output->count++;
		}
	}

	output->last = cur_track_ptr;

	return VOD_OK;
}

static vod_status_t
manifest_utils_get_multilingual_adaptation_sets(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	label_track_count_t* first_label,
	size_t label_count, 
	adaptation_sets_t* output)
{
	adaptation_set_t* first_audio_adaptation;
	adaptation_set_t* cur_adaptation_set;
	adaptation_set_t* adaptation_sets;
	media_track_t** cur_track_ptr;
	media_track_t* last_track;
	media_track_t* cur_track;
	label_track_count_t* last_label = first_label + label_count;
	label_track_count_t* cur_label;
	vod_status_t rc;
	size_t adaptation_sets_count;
	size_t index;

	// get the number of adaptation sets
	adaptation_sets_count = label_count;
	if (media_set->track_count[MEDIA_TYPE_VIDEO] > 0)
	{
		if ((flags & ADAPTATION_SETS_FLAG_FORCE_MUXED) != 0)
		{
			output->count[ADAPTATION_TYPE_MUXED] = 1;
			output->count[ADAPTATION_TYPE_VIDEO] = 0;
			output->count[ADAPTATION_TYPE_AUDIO] = label_count - 1;
		}
		else
		{
			adaptation_sets_count++;
			output->count[ADAPTATION_TYPE_MUXED] = 0;
			output->count[ADAPTATION_TYPE_VIDEO] = 1;
			output->count[ADAPTATION_TYPE_AUDIO] = label_count;
		}
	}
	else
	{
		output->count[ADAPTATION_TYPE_MUXED] = 0;
		output->count[ADAPTATION_TYPE_VIDEO] = 0;
		output->count[ADAPTATION_TYPE_AUDIO] = label_count;
	}

	// allocate the adaptation sets and tracks
	adaptation_sets = vod_alloc(request_context->pool, 
		sizeof(adaptation_sets[0]) * adaptation_sets_count + 
		sizeof(adaptation_sets[0].first[0]) * media_set->total_track_count);
	if (adaptation_sets == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_get_multilingual_adaptation_sets: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_track_ptr = (void*)(adaptation_sets + adaptation_sets_count);
	cur_adaptation_set = adaptation_sets;

	// initialize the video adaptation set
	if (media_set->track_count[MEDIA_TYPE_VIDEO] > 0)
	{
		if (output->count[ADAPTATION_TYPE_MUXED] > 0)
		{
			rc = manifest_utils_get_muxed_adaptation_set(
				request_context,
				media_set,
				flags,
				&first_label->label,
				cur_adaptation_set);
			if (rc != VOD_OK)
			{
				return rc;
			}
			cur_adaptation_set++;
			first_label++;		// do not output this label separately
		}
		else
		{
			cur_adaptation_set->first = cur_track_ptr;
			cur_adaptation_set->count = 0;
			cur_adaptation_set->type = ADAPTATION_TYPE_VIDEO;
			cur_track_ptr += media_set->track_count[MEDIA_TYPE_VIDEO];
			cur_adaptation_set->last = cur_track_ptr;
			cur_adaptation_set++;
		}
	}

	// initialize the audio adaptation sets
	first_audio_adaptation = cur_adaptation_set;

	for (cur_label = first_label; cur_label < last_label; cur_label++)
	{
		cur_adaptation_set->first = cur_track_ptr;
		cur_adaptation_set->count = 0;
		cur_adaptation_set->type = ADAPTATION_TYPE_AUDIO;
		if ((flags & ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK) != 0)
		{
			cur_track_ptr++;
		}
		else
		{
			cur_track_ptr += cur_label->track_count;
		}
		cur_adaptation_set->last = cur_track_ptr;
		cur_adaptation_set++;
	}

	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
	{
		switch (cur_track->media_info.media_type)
		{
		case MEDIA_TYPE_AUDIO:
			if (cur_track->media_info.label.len == 0)
			{
				continue;
			}

			// find the label index
			for (cur_label = first_label, index = 0; cur_label < last_label; cur_label++, index++)
			{
				if (vod_str_equals(cur_label->label, cur_track->media_info.label))
				{
					break;
				}
			}

			if (cur_label >= last_label)
			{
				continue;
			}

			// find the adaptation set
			cur_adaptation_set = first_audio_adaptation + index;

			if ((flags & ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK) != 0 &&
				cur_adaptation_set->count != 0)
			{
				continue;
			}
			break;

		case MEDIA_TYPE_VIDEO:
			// in forced muxed mode, all video tracks were already added
			if (output->count[ADAPTATION_TYPE_MUXED] > 0)
			{
				continue;
			}

			cur_adaptation_set = adaptation_sets;
			break;

		default:
			continue;
		}

		// add the track to the adaptation set
		cur_adaptation_set->first[cur_adaptation_set->count++] = cur_track;
	}

	output->first = adaptation_sets;
	output->last = adaptation_sets + adaptation_sets_count;
	output->total_count = adaptation_sets_count;

	return VOD_OK;
}

static vod_status_t
manifest_utils_get_unmuxed_adaptation_sets(
	request_context_t* request_context,
	media_set_t* media_set,
	adaptation_sets_t* output)
{
	adaptation_set_t* adaptation_set_by_media_type[MEDIA_TYPE_COUNT];
	adaptation_set_t* cur_adaptation_set;
	adaptation_set_t* adaptation_sets;
	media_track_t** cur_track_ptr;
	media_track_t* last_track;
	media_track_t* cur_track;
	size_t adaptation_sets_count;
	uint32_t media_type;

	// get the number of adaptation sets
	adaptation_sets_count = 0;
	output->count[ADAPTATION_TYPE_MUXED] = 0;

	if (media_set->track_count[MEDIA_TYPE_VIDEO] > 0)
	{
		adaptation_sets_count++;
		output->count[ADAPTATION_TYPE_VIDEO] = 1;
	}
	else
	{
		output->count[ADAPTATION_TYPE_VIDEO] = 0;
	}

	if (media_set->track_count[MEDIA_TYPE_AUDIO] > 0)
	{
		adaptation_sets_count++;
		output->count[ADAPTATION_TYPE_AUDIO] = 1;
	}
	else
	{
		output->count[ADAPTATION_TYPE_AUDIO] = 0;
	}

	// allocate the adaptation sets
	adaptation_sets = vod_alloc(request_context->pool, 
		sizeof(adaptation_sets[0]) * adaptation_sets_count + 
		sizeof(adaptation_sets[0].first[0]) * media_set->total_track_count);
	if (adaptation_sets == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_get_unmuxed_adaptation_sets: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_track_ptr = (void*)(adaptation_sets + adaptation_sets_count);

	// initialize the adaptation sets
	cur_adaptation_set = adaptation_sets;
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		if (media_set->track_count[media_type] == 0)
		{
			continue;
		}

		adaptation_set_by_media_type[media_type] = cur_adaptation_set;
		cur_adaptation_set->first = cur_track_ptr;
		cur_adaptation_set->count = 0;
		cur_adaptation_set->type = media_type;
		cur_track_ptr += media_set->track_count[media_type];
		cur_adaptation_set->last = cur_track_ptr;
		cur_adaptation_set++;
	}

	// add the tracks to the adaptation sets
	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (cur_track = media_set->filtered_tracks; cur_track < last_track; cur_track++)
	{
		media_type = cur_track->media_info.media_type;
		if (media_type >= MEDIA_TYPE_COUNT) // MEDIA_TYPE_NONE
		{
			continue;
		}

		cur_adaptation_set = adaptation_set_by_media_type[media_type];
		cur_adaptation_set->first[cur_adaptation_set->count++] = cur_track;
	}

	output->first = adaptation_sets;
	output->last = adaptation_sets + adaptation_sets_count;
	output->total_count = adaptation_sets_count;

	return VOD_OK;
}

vod_status_t
manifest_utils_get_adaptation_sets(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	adaptation_sets_t* output)
{
	label_track_count_t* first_label;
	label_track_count_t* last_label;
	label_track_count_t temp_label;
	size_t label_count;
	vod_status_t rc;

	rc = manifest_utils_get_unique_labels(
		request_context,
		media_set,
		&first_label,
		&label_count);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (label_count > 1)
	{
		if ((flags & ADAPTATION_SETS_FLAG_DEFAULT_LANG_LAST) != 0)
		{
			last_label = first_label + (label_count - 1);
			temp_label = *first_label;
			*first_label = *last_label;
			*last_label = temp_label;
		}

		rc = manifest_utils_get_multilingual_adaptation_sets(
			request_context,
			media_set,
			flags,
			first_label,
			label_count, 
			output);
	}
	else if ((flags & (ADAPTATION_SETS_FLAG_MUXED | ADAPTATION_SETS_FLAG_FORCE_MUXED)) != 0)
	{
		output->first = vod_alloc(request_context->pool, sizeof(output->first[0]));
		if (output->first == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"manifest_utils_get_adaptation_sets: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}

		output->last = output->first + 1;
		output->count[ADAPTATION_TYPE_MUXED] = 1;
		output->count[ADAPTATION_TYPE_AUDIO] = 0;
		output->count[ADAPTATION_TYPE_VIDEO] = 0;
		output->total_count = 1;

		rc = manifest_utils_get_muxed_adaptation_set(
			request_context,
			media_set,
			flags, 
			NULL,
			output->first);
	}
	else
	{
		rc = manifest_utils_get_unmuxed_adaptation_sets(
			request_context,
			media_set,
			output);
	}

	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
