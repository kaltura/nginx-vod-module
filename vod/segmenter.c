#include "segmenter.h"

// constants
#define MAX_SEGMENT_COUNT (100000)

// typedefs
typedef struct {
	segmenter_conf_t* conf;
	uint32_t segment_index;
	uint32_t segment_count;
	uint32_t last_boundary;
} segmenter_boundary_iterator_context_t;

vod_status_t 
segmenter_init_config(segmenter_conf_t* conf, vod_pool_t* pool)
{
	vod_str_t* cur_str;
	uint32_t* buffer;
	uint32_t cur_pos = 0;
	uint32_t i;
	int32_t cur_duration;

	conf->max_segment_duration = conf->segment_duration;

	if (conf->get_segment_durations == segmenter_get_segment_durations_accurate)
	{
		conf->parse_type = PARSE_FLAG_FRAMES_DURATION;
		if (conf->align_to_key_frames)
		{
			conf->parse_type |= PARSE_FLAG_FRAMES_IS_KEY;
		}
	}
	else
	{
		conf->parse_type = 0;
	}

	if (conf->bootstrap_segments == NULL)
	{
		conf->bootstrap_segments_count = 0;
		conf->bootstrap_segments_durations = NULL;
		conf->bootstrap_segments_total_duration = 0;
		conf->bootstrap_segments_start = NULL;
		conf->bootstrap_segments_mid = NULL;
		conf->bootstrap_segments_end = NULL;
		return VOD_OK;
	}

	conf->bootstrap_segments_count = conf->bootstrap_segments->nelts;
	buffer = vod_alloc(pool, 4 * conf->bootstrap_segments_count * sizeof(uint32_t));
	if (buffer == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	conf->bootstrap_segments_durations = buffer;
	conf->bootstrap_segments_start =     buffer + conf->bootstrap_segments_count;
	conf->bootstrap_segments_mid =       buffer + 2 * conf->bootstrap_segments_count;
	conf->bootstrap_segments_end =       buffer + 3 * conf->bootstrap_segments_count;

	for (i = 0; i < conf->bootstrap_segments_count; i++)
	{
		cur_str = (vod_str_t*)conf->bootstrap_segments->elts + i;
		
		cur_duration = vod_atoi(cur_str->data, cur_str->len);
		if (cur_duration <= 0)
		{
			return VOD_BAD_DATA;
		}
		
		conf->bootstrap_segments_durations[i] = cur_duration;
		conf->bootstrap_segments_start[i] = cur_pos;
		conf->bootstrap_segments_mid[i] = cur_pos + conf->bootstrap_segments_durations[i] / 2;
		cur_pos += conf->bootstrap_segments_durations[i];
		conf->bootstrap_segments_end[i] = cur_pos;
	}
	conf->bootstrap_segments_total_duration = cur_pos;

	return VOD_OK;
}

uint32_t
segmenter_get_segment_count_last_short(segmenter_conf_t* conf, uint64_t duration_millis)
{
	uint32_t result;

	if (duration_millis == 0)
	{
		return 0;
	}

	if (duration_millis > conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;

		if (duration_millis > (uint64_t)conf->segment_duration * (UINT_MAX - conf->bootstrap_segments_count - 2))
		{
			return INVALID_SEGMENT_COUNT;
		}

		result = conf->bootstrap_segments_count + vod_div_ceil(duration_millis, conf->segment_duration);
	}
	else
	{
		for (result = 1; result < conf->bootstrap_segments_count && duration_millis > conf->bootstrap_segments_start[result]; result++);
	}

	return result;
}

uint32_t
segmenter_get_segment_count_last_long(segmenter_conf_t* conf, uint64_t duration_millis)
{
	uint32_t result;

	if (duration_millis == 0)
	{
		return 0;
	}

	if (duration_millis >= conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;

		if (duration_millis > (uint64_t)conf->segment_duration * (UINT_MAX - conf->bootstrap_segments_count - 2))
		{
			return INVALID_SEGMENT_COUNT;
		}

		result = conf->bootstrap_segments_count + duration_millis / conf->segment_duration;
		if (result < 1)
		{
			result = 1;
		}
	}
	else
	{
		for (result = 1; result < conf->bootstrap_segments_count && duration_millis >= conf->bootstrap_segments_end[result]; result++);
	}

	return result;
}

uint32_t
segmenter_get_segment_count_last_rounded(segmenter_conf_t* conf, uint64_t duration_millis)
{
	uint32_t result;

	if (duration_millis == 0)
	{
		return 0;
	}

	if (duration_millis >= conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;

		if (duration_millis > (uint64_t)conf->segment_duration * (UINT_MAX - conf->bootstrap_segments_count - 2))
		{
			return INVALID_SEGMENT_COUNT;
		}

		result = conf->bootstrap_segments_count + (duration_millis + conf->segment_duration / 2) / conf->segment_duration;
		if (result < 1)
		{
			result = 1;
		}
	}
	else
	{
		for (result = 1; result < conf->bootstrap_segments_count && duration_millis >= conf->bootstrap_segments_mid[result]; result++);
	}

	return result;
}

static void
segmenter_get_start_end_offsets(segmenter_conf_t* conf, uint32_t segment_index, uint64_t* start, uint64_t* end)
{
	if (segment_index < conf->bootstrap_segments_count)
	{
		*start = conf->bootstrap_segments_start[segment_index];
		*end = conf->bootstrap_segments_end[segment_index];
	}
	else
	{
		*start = conf->bootstrap_segments_total_duration + (segment_index - conf->bootstrap_segments_count) * conf->segment_duration;
		*end = *start + conf->segment_duration;
	}
}

uint32_t
segmenter_get_segment_index_no_discontinuity(
	segmenter_conf_t* conf,
	uint64_t time_millis)
{
	uint32_t* cur_end_offset;
	uint32_t result;

	// regular segments
	if (time_millis >= conf->bootstrap_segments_total_duration)
	{
		return conf->bootstrap_segments_count +
			(time_millis - conf->bootstrap_segments_total_duration) / conf->segment_duration;
	}

	// bootstrap segments
	result = 0;
	for (cur_end_offset = conf->bootstrap_segments_end; time_millis >= *cur_end_offset; cur_end_offset++)
	{
		result++;
	}
	return result;
}

vod_status_t
segmenter_get_segment_index_discontinuity(
	request_context_t* request_context,
	segmenter_conf_t* conf, 
	uint32_t initial_segment_index,
	uint32_t* clip_durations,
	uint32_t total_clip_count,
	uint64_t time_millis, 
	uint32_t* result)
{
	uint64_t clip_start_offset;
	uint64_t prev_clips_duration = 0;
	uint64_t ignore;
	uint32_t* cur_duration;
	uint32_t* end_duration = clip_durations + total_clip_count;
	uint32_t clip_segment_limit;
	uint32_t segment_index = initial_segment_index;

	for (cur_duration = clip_durations; ; cur_duration++)
	{
		if (cur_duration >= end_duration)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_segment_index_discontinuity: invalid segment time %uD", time_millis);
			return VOD_BAD_REQUEST;
		}

		// get the clip start offset
		segmenter_get_start_end_offsets(conf, segment_index, &clip_start_offset, &ignore);

		// get segment limit for the current clip
		clip_segment_limit = conf->get_segment_count(conf, clip_start_offset + *cur_duration);
		if (clip_segment_limit == INVALID_SEGMENT_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_segment_index_discontinuity: segment count is invalid");
			return VOD_BAD_DATA;
		}

		if (clip_segment_limit <= segment_index)
		{
			clip_segment_limit = segment_index + 1;
		}

		// check whether the time stamp falls within the current clip
		if (time_millis < prev_clips_duration + *cur_duration)
		{
			break;
		}

		prev_clips_duration += *cur_duration;

		// move to the next clip
		segment_index = clip_segment_limit;
	}

	// check bootstrap segments
	time_millis -= prev_clips_duration;

	for (; segment_index < conf->bootstrap_segments_count; segment_index++)
	{
		if (time_millis < conf->bootstrap_segments_durations[segment_index])
		{
			*result = segment_index;
			return VOD_OK;
		}

		time_millis -= conf->bootstrap_segments_durations[segment_index];
	}

	*result = segment_index + time_millis / conf->segment_duration;
	return VOD_OK;
}

vod_status_t
segmenter_get_start_end_ranges_no_discontinuity(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	uint32_t segment_index,
	uint32_t* clip_durations,
	uint32_t total_clip_count,
	uint64_t start_time,
	uint64_t end_time,
	uint64_t last_segment_end,
	get_clip_ranges_result_t* result)
{
	uint64_t clip_start_offset = start_time;
	uint64_t next_start_offset;
	uint64_t start;
	uint64_t end;
	media_range_t* cur_clip_range;
	uint32_t* end_duration = clip_durations + total_clip_count;
	uint32_t* cur_duration;
	uint32_t segment_count;
	uint32_t index;

	result->clip_index_segment_index = 0;
	result->first_clip_segment_index = 0;

	// get the segment count
	segment_count = conf->get_segment_count(conf, end_time);
	if (segment_count == INVALID_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: segment count is invalid for total duration %uL", end_time);
		return VOD_BAD_DATA;
	}

	if (segment_index >= segment_count)
	{
		result->clip_count = 0;
		result->min_clip_index = 1;
		result->max_clip_index = 0;
		return VOD_OK;
	}

	// get the start / end offsets
	segmenter_get_start_end_offsets(
		conf,
		segment_index,
		&start,
		&end);

	if (start < start_time)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: segment start time %uL is less than sequence start time %uL",
			start, start_time);
		return VOD_BAD_REQUEST;
	}

	if (segment_index + 1 >= segment_count)
	{
		end = last_segment_end;
	}

	// find min/max clip indexes and initial sequence offset
	result->min_clip_index = INVALID_CLIP_INDEX;
	result->max_clip_index = total_clip_count - 1;

	for (cur_duration = clip_durations; cur_duration < end_duration; cur_duration++, clip_start_offset = next_start_offset)
	{
		next_start_offset = clip_start_offset + *cur_duration;
		if (start >= next_start_offset)
		{
			continue;
		}

		if (start >= clip_start_offset)
		{
			result->min_clip_index = cur_duration - clip_durations;
			result->initial_sequence_offset = clip_start_offset;
		}

		if (end <= next_start_offset)
		{
			result->max_clip_index = cur_duration - clip_durations;
			break;
		}
	}

	if (result->min_clip_index == INVALID_CLIP_INDEX)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: invalid segment index %uD", segment_index);
		return VOD_BAD_REQUEST;
	}

	// allocate the clip ranges
	result->clip_count = result->max_clip_index - result->min_clip_index + 1;
	cur_clip_range = vod_alloc(request_context->pool, sizeof(result->clip_ranges[0]) * result->clip_count);
	if (cur_clip_range == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->clip_ranges = cur_clip_range;

	// initialize the clip ranges
	start -= result->initial_sequence_offset;
	end -= result->initial_sequence_offset;
	for (index = result->min_clip_index;; index++, cur_clip_range++)
	{
		cur_clip_range->timescale = 1000;
		cur_clip_range->start = start;
		if (index >= result->max_clip_index)
		{
			cur_clip_range->end = end;
			break;
		}

		cur_clip_range->end = clip_durations[index];

		start = 0;
		end -= clip_durations[index];
	}
	
	result->initial_sequence_offset -= start_time;

	return VOD_OK;
}

vod_status_t
segmenter_get_start_end_ranges_discontinuity(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	uint32_t clip_index,
	uint32_t segment_index,
	uint32_t initial_segment_index,
	uint32_t* clip_durations,
	uint32_t total_clip_count,
	uint64_t total_duration,
	get_clip_ranges_result_t* result)
{
	uint64_t clip_start_offset;
	uint64_t start;
	uint64_t end;
	uint64_t ignore;
	uint32_t* end_duration = clip_durations + total_clip_count;
	uint32_t* cur_duration;
	uint32_t cur_segment_limit;
	uint32_t last_segment_limit = initial_segment_index;
	uint32_t clip_index_segment_index = 0;
	media_range_t* cur_clip_range;
	uint64_t prev_clips_duration = 0;

	for (cur_duration = clip_durations; ; cur_duration++)
	{
		if (cur_duration >= end_duration)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_discontinuity: invalid segment index %uD or clip index", segment_index);
			return VOD_BAD_REQUEST;
		}

		// get the clip start offset
		segmenter_get_start_end_offsets(conf, last_segment_limit, &clip_start_offset, &ignore);

		// get segment limit for the current clip
		cur_segment_limit = conf->get_segment_count(conf, clip_start_offset + *cur_duration);
		if (cur_segment_limit == INVALID_SEGMENT_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_discontinuity: invalid segment count");
			return VOD_BAD_DATA;
		}

		if (cur_segment_limit <= last_segment_limit)
		{
			cur_segment_limit = last_segment_limit + 1;
		}

		if (clip_index == 1)
		{
			clip_index_segment_index = cur_segment_limit - initial_segment_index;
			segment_index += clip_index_segment_index;
		}
		
		if (clip_index > 0 && clip_index != INVALID_CLIP_INDEX)
		{
			clip_index--;
		}
		else if (segment_index < cur_segment_limit)
		{
			// the segment index is within this clip, break
			break;
		}

		// move to the next clip
		prev_clips_duration += *cur_duration;
		last_segment_limit = cur_segment_limit;
	}

	if (segment_index < last_segment_limit)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_discontinuity: segment index %uD smaller than last segment index %uD", 
			segment_index, last_segment_limit);
		return VOD_BAD_REQUEST;
	}

	// get start / end position relative to the clip start
	segmenter_get_start_end_offsets(
		conf,
		segment_index,
		&start,
		&end);

	start -= clip_start_offset;
	if (segment_index + 1 >= cur_segment_limit)
	{
		end = *cur_duration;		// last segment in clip
	}
	else
	{
		end -= clip_start_offset;
	}

	// initialize the clip range
	cur_clip_range = vod_alloc(request_context->pool, sizeof(cur_clip_range[0]));
	if (cur_clip_range == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_start_end_ranges_discontinuity: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_clip_range->timescale = 1000;
	cur_clip_range->start = start;
	cur_clip_range->end = end;

	// initialize the result
	result->initial_sequence_offset = prev_clips_duration;
	result->min_clip_index = result->max_clip_index = cur_duration - clip_durations;
	result->clip_count = 1;
	result->clip_ranges = cur_clip_range;
	result->clip_index_segment_index = clip_index_segment_index;
	result->first_clip_segment_index = last_segment_limit;

	return VOD_OK;
}

static vod_status_t
segmenter_get_segment_durations_estimate_internal(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	uint32_t* clip_durations,
	uint32_t total_clip_count,
	uint64_t cur_clip_duration,
	segment_durations_t* result)
{
	segment_duration_item_t* cur_item;
	uint64_t clip_start_offset;
	uint64_t ignore;
	uint64_t clip_offset;
	uint32_t* end_duration = clip_durations + total_clip_count;
	uint32_t* cur_duration = clip_durations;
	uint32_t bootstrap_segment_limit;
	uint32_t segment_index = media_set->initial_segment_index;
	uint32_t clip_segment_limit;
	uint32_t segment_duration;
	bool_t discontinuity;

	// allocate the result buffer
	result->items = vod_alloc(request_context->pool, sizeof(result->items[0]) * (conf->bootstrap_segments_count + 2 * total_clip_count));
	if (result->items == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_segment_durations_estimate_internal: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_item = result->items - 1;

	discontinuity = FALSE;
	for (;;)
	{
		// find the clip start offset
		segmenter_get_start_end_offsets(conf, segment_index, &clip_start_offset, &ignore);

		// get segment limit for the current clip
		clip_segment_limit = conf->get_segment_count(conf, clip_start_offset + cur_clip_duration);
		if (clip_segment_limit == INVALID_SEGMENT_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_segment_durations_estimate_internal: segment count is invalid");
			return VOD_BAD_DATA;
		}

		if (clip_segment_limit <= segment_index)
		{
			clip_segment_limit = segment_index + 1;
		}

		clip_offset = 0;

		// bootstrap segments
		bootstrap_segment_limit = vod_min(clip_segment_limit - 1, conf->bootstrap_segments_count);
		for (; segment_index < bootstrap_segment_limit; segment_index++)
		{
			segment_duration = conf->bootstrap_segments_durations[segment_index];
			if (cur_item < result->items || segment_duration != cur_item->duration || discontinuity)
			{
				cur_item++;
				cur_item->repeat_count = 0;
				cur_item->segment_index = segment_index;
				cur_item->duration = segment_duration;
				cur_item->discontinuity = discontinuity;
				discontinuity = FALSE;
			}
			cur_item->repeat_count++;

			clip_offset += segment_duration;
		}

		// remaining segments
		if (segment_index + 1 < clip_segment_limit)
		{
			segment_duration = conf->segment_duration;
			if (cur_item < result->items || segment_duration != cur_item->duration || discontinuity)
			{
				cur_item++;
				cur_item->repeat_count = 0;
				cur_item->segment_index = segment_index;
				cur_item->duration = segment_duration;
				cur_item->discontinuity = discontinuity;
				discontinuity = FALSE;
			}
			cur_item->repeat_count += clip_segment_limit - segment_index - 1;

			clip_offset += (uint64_t)segment_duration * (clip_segment_limit - segment_index - 1);
			segment_index = clip_segment_limit - 1;
		}

		// last segment
		if (segment_index < clip_segment_limit && clip_offset < cur_clip_duration)
		{
			segment_duration = cur_clip_duration - clip_offset;
			if (cur_item < result->items || segment_duration != cur_item->duration || discontinuity)
			{
				cur_item++;
				cur_item->repeat_count = 0;
				cur_item->segment_index = segment_index;
				cur_item->duration = segment_duration;
				cur_item->discontinuity = discontinuity;
			}
			cur_item->repeat_count++;
			
			segment_index = clip_segment_limit;
		}

		// move to the next clip
		cur_duration++;
		if (cur_duration >= end_duration)
		{
			break;
		}

		cur_clip_duration = *cur_duration;

		// update clip_start_offset
		discontinuity = TRUE;
	}

	// finalize the result
	result->segment_count = clip_segment_limit - media_set->initial_segment_index;
	if (result->segment_count > MAX_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: segment count %uD is invalid", result->segment_count);
		return VOD_BAD_MAPPING;
	}

	result->item_count = cur_item + 1 - result->items;
	result->timescale = 1000;
	result->discontinuities = total_clip_count - 1;

	return VOD_OK;
}

vod_status_t
segmenter_get_segment_durations_estimate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	uint32_t media_type,
	segment_durations_t* result)
{
	media_track_t** longest_track;
	media_sequence_t* sequences_end;
	media_sequence_t* cur_sequence;
	uint64_t duration_millis;

	if (media_set->durations != NULL)
	{
		duration_millis = media_set->total_duration;

		if (media_set->use_discontinuity)
		{
			result->start_time = media_set->first_clip_time;
			result->end_time = media_set->first_clip_time + duration_millis;

			return segmenter_get_segment_durations_estimate_internal(
				request_context,
				conf,
				media_set,
				media_set->durations,
				media_set->total_clip_count,
				media_set->durations[0],
				result);
		}

		// no discontinuity - treat it like a single clip
	}
	else
	{
		duration_millis = 0;

		if (sequence != NULL)
		{
			cur_sequence = sequence;
			sequences_end = sequence + 1;
		}
		else
		{
			cur_sequence = media_set->sequences;
			sequences_end = media_set->sequences_end;
		}

		for (; cur_sequence < sequences_end; cur_sequence++)
		{
			longest_track = cur_sequence->filtered_clips[0].longest_track;

			if (longest_track[MEDIA_TYPE_VIDEO] != NULL && 
				longest_track[MEDIA_TYPE_VIDEO]->media_info.duration_millis > duration_millis &&
				(media_type == MEDIA_TYPE_NONE || media_type == MEDIA_TYPE_VIDEO))
			{
				duration_millis = longest_track[MEDIA_TYPE_VIDEO]->media_info.duration_millis;
			}

			if (longest_track[MEDIA_TYPE_AUDIO] != NULL &&
				longest_track[MEDIA_TYPE_AUDIO]->media_info.duration_millis > duration_millis &&
				(media_type == MEDIA_TYPE_NONE || media_type == MEDIA_TYPE_AUDIO))
			{
				duration_millis = longest_track[MEDIA_TYPE_AUDIO]->media_info.duration_millis;
			}
		}
	}

	result->start_time = media_set->first_clip_time;
	result->end_time = media_set->first_clip_time + duration_millis;

	return segmenter_get_segment_durations_estimate_internal(
		request_context,
		conf,
		media_set,
		NULL,
		1,
		duration_millis,
		result);
}

static void
segmenter_boundary_iterator_init(segmenter_boundary_iterator_context_t* context, segmenter_conf_t* conf, uint32_t segment_count)
{
	context->conf = conf;
	context->segment_index = 0;
	context->segment_count = segment_count;
	context->last_boundary = context->conf->bootstrap_segments_total_duration;
}

static uint32_t
segmenter_boundary_iterator_next(segmenter_boundary_iterator_context_t* context)
{
	uint32_t result;

	if (context->segment_index + 1 >= context->segment_count)
	{
		return UINT_MAX;
	}

	if (context->segment_index < context->conf->bootstrap_segments_count)
	{
		result = context->conf->bootstrap_segments_end[context->segment_index];
	}
	else
	{
		context->last_boundary += context->conf->segment_duration;
		result = context->last_boundary;
	}

	context->segment_index++;
	return result;
}

static void
segmenter_boundary_iterator_skip(segmenter_boundary_iterator_context_t* context, uint32_t count)
{
	context->segment_index = vod_min(context->segment_index + count, context->segment_count);

	if (context->segment_index > context->conf->bootstrap_segments_count)
	{
		context->last_boundary = context->conf->bootstrap_segments_total_duration +
			(context->segment_index - context->conf->bootstrap_segments_count) * context->conf->segment_duration;
	}
}

vod_status_t 
segmenter_get_segment_durations_accurate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	uint32_t media_type,
	segment_durations_t* result)
{
	segmenter_boundary_iterator_context_t boundary_iterator;
	media_track_t* cur_track;
	media_track_t* last_track;
	media_track_t* main_track = NULL;
	media_track_t* longest_track = NULL;
	segment_duration_item_t* cur_item;
	media_sequence_t* sequences_end;
	media_sequence_t* cur_sequence;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	uint64_t total_duration;
	uint32_t segment_index = 0;
	uint64_t accum_duration = 0;
	uint64_t segment_start = 0;
	uint64_t segment_limit_millis;
	uint64_t segment_limit;
	uint64_t cur_duration;
	uint32_t duration_millis;
	bool_t align_to_key_frames;

	if (media_set->durations != NULL)
	{
		// in case of a playlist fall back to estimate
		return segmenter_get_segment_durations_estimate(
			request_context,
			conf,
			media_set,
			sequence,
			media_type,
			result);
	}

	// get the maximum duration and main track (=first video track if exists, or first audio track otherwise)
	if (sequence != NULL)
	{
		cur_sequence = sequence;
		sequences_end = sequence + 1;
	}
	else
	{
		cur_sequence = media_set->sequences;
		sequences_end = media_set->sequences_end;
	}

	duration_millis = 0;
	for (; cur_sequence < sequences_end; cur_sequence++)
	{
		last_track = cur_sequence->filtered_clips[0].last_track;
		for (cur_track = cur_sequence->filtered_clips[0].first_track; cur_track < last_track; cur_track++)
		{
			if (media_type != MEDIA_TYPE_NONE && cur_track->media_info.media_type != media_type)
			{
				continue;
			}

			if (main_track == NULL || 
				(main_track->media_info.media_type == MEDIA_TYPE_AUDIO && 
				 cur_track->media_info.media_type == MEDIA_TYPE_VIDEO))
			{
				main_track = cur_track;
			}

			if (cur_track->media_info.duration_millis > duration_millis)
			{
				longest_track = cur_track;
				duration_millis = cur_track->media_info.duration_millis;
			}
		}
	}

	if (main_track == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: didnt get any tracks");
		return VOD_UNEXPECTED;
	}

	if (main_track->media_info.media_type == MEDIA_TYPE_AUDIO && media_set->audio_filtering_needed)
	{
		// the main track is audio and the filters were not applied to it, fall back to estimate
		return segmenter_get_segment_durations_estimate(
			request_context,
			conf,
			media_set,
			sequence,
			media_type,
			result);
	}

	// get the segment count
	result->segment_count = conf->get_segment_count(conf, duration_millis);
	if (result->segment_count > MAX_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: segment count %uD is invalid", result->segment_count);
		return VOD_BAD_DATA;
	}

	// allocate the result buffer
	result->items = vod_alloc(request_context->pool, sizeof(*result->items) * result->segment_count);
	if (result->items == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->timescale = main_track->media_info.timescale;
	result->discontinuities = 0;

	cur_item = result->items - 1;
	last_frame = main_track->last_frame;
	cur_frame = main_track->first_frame;

	align_to_key_frames = conf->align_to_key_frames && main_track->media_info.media_type == MEDIA_TYPE_VIDEO;

	// bootstrap segments
	if (conf->bootstrap_segments_count > 0)
	{
		segment_limit = rescale_time(conf->bootstrap_segments_end[0], 1000, result->timescale);

		for (; cur_frame < last_frame; cur_frame++)
		{
			while (accum_duration >= segment_limit && segment_index + 1 < result->segment_count &&
				(!align_to_key_frames || cur_frame->key_frame))
			{
				// get the current duration and update to array
				cur_duration = accum_duration - segment_start;
				if (cur_item < result->items || cur_duration != cur_item->duration)
				{
					cur_item++;
					cur_item->repeat_count = 0;
					cur_item->segment_index = segment_index;
					cur_item->duration = cur_duration;
					cur_item->discontinuity = FALSE;
				}
				cur_item->repeat_count++;

				// move to the next segment
				segment_start = accum_duration;
				segment_index++;
				if (segment_index >= conf->bootstrap_segments_count)
				{
					goto post_bootstrap;
				}
				segment_limit = rescale_time(conf->bootstrap_segments_end[segment_index], 1000, result->timescale);
			}
			accum_duration += cur_frame->duration;
		}
	}

post_bootstrap:

	// remaining segments
	segment_limit_millis = conf->bootstrap_segments_total_duration + conf->segment_duration;
	segment_limit = rescale_time(segment_limit_millis, 1000, result->timescale);

	for (; cur_frame < last_frame; cur_frame++)
	{
		while (accum_duration >= segment_limit && segment_index + 1 < result->segment_count &&
			(!align_to_key_frames || cur_frame->key_frame))
		{
			// get the current duration and update to array
			cur_duration = accum_duration - segment_start;
			if (cur_item < result->items || cur_duration != cur_item->duration)
			{
				cur_item++;
				cur_item->repeat_count = 0;
				cur_item->segment_index = segment_index;
				cur_item->duration = cur_duration;
				cur_item->discontinuity = FALSE;
			}
			cur_item->repeat_count++;

			// move to the next segment
			segment_index++;
			segment_start = accum_duration;
			segment_limit_millis += conf->segment_duration;
			segment_limit = rescale_time(segment_limit_millis, 1000, result->timescale);
		}
		accum_duration += cur_frame->duration;
	}
	
	// in case the main video track is shorter than the audio track, add the estimated durations of the remaining audio-only segments
	if (main_track->media_info.duration_millis < duration_millis && 
		!align_to_key_frames)
	{
		segmenter_boundary_iterator_init(&boundary_iterator, conf, result->segment_count);
		segmenter_boundary_iterator_skip(&boundary_iterator, segment_index);

		total_duration = rescale_time(longest_track->media_info.duration, longest_track->media_info.timescale, result->timescale);

		while (accum_duration < total_duration && 
			segment_index + 1 < result->segment_count)
		{
			segment_limit_millis = segmenter_boundary_iterator_next(&boundary_iterator);
			segment_limit = rescale_time(segment_limit_millis, 1000, result->timescale);
			segment_limit = vod_min(segment_limit, total_duration);

			accum_duration = segment_limit;

			cur_duration = accum_duration - segment_start;
			if (cur_item < result->items || cur_duration != cur_item->duration)
			{
				cur_item++;
				cur_item->repeat_count = 0;
				cur_item->segment_index = segment_index;
				cur_item->duration = cur_duration;
				cur_item->discontinuity = FALSE;
			}
			cur_item->repeat_count++;
			
			// move to the next segment
			segment_index++;
			segment_start = accum_duration;
		}

		accum_duration = total_duration;
	}

	// add the last segment / empty segments after the last keyframe (in case align to key frames is on)
	while (segment_index < result->segment_count)
	{
		// get the current duration and update to array
		cur_duration = accum_duration - segment_start;
		if (cur_item < result->items || cur_duration != cur_item->duration)
		{
			cur_item++;
			cur_item->repeat_count = 0;
			cur_item->segment_index = segment_index;
			cur_item->duration = cur_duration;
			cur_item->discontinuity = FALSE;
		}
		cur_item->repeat_count++;

		// move to the next segment
		segment_index++;
		segment_start = accum_duration;
	}

	result->item_count = cur_item + 1 - result->items;

	// remove any empty segments from the end
	if (result->item_count > 0 && cur_item->duration == 0)
	{
		result->item_count--;
		result->segment_count -= cur_item->repeat_count;
	}

	result->start_time = 0;
	result->end_time = duration_millis;

	return VOD_OK;
}
