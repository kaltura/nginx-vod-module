#include "segmenter.h"

// constants
#define MAX_SEGMENT_COUNT (100000)

// typedefs
typedef struct {
	uint64_t start_time;
	uint32_t start_clip_offset;
	uint32_t start_clip_index;
	uint64_t end_time;
	uint32_t end_clip_offset;
	uint32_t end_clip_index;
} live_window_start_end_t;

typedef struct {
	segment_durations_t* result;
	segment_duration_item_t* cur_item;
	bool_t discontinuity;
	uint32_t segment_index;

	align_to_key_frames_context_t align;
	uint64_t cur_time;
	uint64_t clip_end_time;
	uint64_t aligned_time;
} segmenter_get_segment_durations_context_t;

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

	if (conf->segment_duration < MIN_SEGMENT_DURATION || 
		conf->segment_duration > MAX_SEGMENT_DURATION)
	{
		return VOD_BAD_DATA;
	}

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

	conf->max_bootstrap_segment_duration = 0;

	if (conf->bootstrap_segments == NULL)
	{
		conf->max_segment_duration = conf->segment_duration;
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
		if (cur_duration < MIN_SEGMENT_DURATION)
		{
			return VOD_BAD_DATA;
		}
		
		conf->bootstrap_segments_durations[i] = cur_duration;
		conf->bootstrap_segments_start[i] = cur_pos;
		conf->bootstrap_segments_mid[i] = cur_pos + conf->bootstrap_segments_durations[i] / 2;
		cur_pos += conf->bootstrap_segments_durations[i];
		conf->bootstrap_segments_end[i] = cur_pos;

		if ((uint32_t)cur_duration > conf->max_bootstrap_segment_duration)
		{
			conf->max_bootstrap_segment_duration = cur_duration;
		}
	}

	conf->max_segment_duration = vod_max(conf->segment_duration, 
		conf->max_bootstrap_segment_duration);

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
segmenter_get_start_offset(segmenter_conf_t* conf, uint32_t segment_index, uint64_t* start)
{
	if (segment_index < conf->bootstrap_segments_count)
	{
		*start = conf->bootstrap_segments_start[segment_index];
	}
	else
	{
		*start = conf->bootstrap_segments_total_duration + 
			(segment_index - conf->bootstrap_segments_count) * conf->segment_duration;
	}
}

static void
segmenter_get_end_offset(segmenter_conf_t* conf, uint32_t segment_index, uint64_t* end)
{
	if (segment_index < conf->bootstrap_segments_count)
	{
		*end = conf->bootstrap_segments_end[segment_index];
	}
	else
	{
		*end = conf->bootstrap_segments_total_duration + 
			(segment_index - conf->bootstrap_segments_count + 1) * conf->segment_duration;
	}
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
		*start = conf->bootstrap_segments_total_duration + 
			(segment_index - conf->bootstrap_segments_count) * conf->segment_duration;
		*end = *start + conf->segment_duration;
	}
}

int64_t 
segmenter_align_to_key_frames(
	align_to_key_frames_context_t* context, 
	int64_t offset, 
	int64_t limit)
{
	int64_t cur_duration;

	while (context->offset < offset)
	{
		if ((void*)context->cur_pos >= context->part->last)
		{
			if (context->part->next == NULL)
			{
				return limit;
			}

			context->part = context->part->next;
			context->cur_pos = context->part->first;
		}

		cur_duration = *context->cur_pos++;

		context->offset += cur_duration;
		if (context->offset >= limit)
		{
			return limit;
		}
	}

	return vod_min(context->offset, limit);
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
	media_clip_timing_t* timing,
	uint64_t time_millis, 
	uint32_t* result)
{
	uint64_t clip_start_offset;
	uint32_t* cur_duration;
	uint32_t* end_duration = timing->durations + timing->total_count;
	uint32_t clip_segment_limit;
	uint32_t segment_index = initial_segment_index;
	uint64_t clip_time;
	uint64_t* cur_clip_time = timing->times;

	for (cur_duration = timing->durations; ; cur_duration++)
	{
		if (cur_duration >= end_duration)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_segment_index_discontinuity: invalid segment time %uD (1)", time_millis);
			return VOD_BAD_REQUEST;
		}

		// check whether the timestamp falls within the current clip
		clip_time = *cur_clip_time++;

		if (time_millis < clip_time)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_segment_index_discontinuity: invalid segment time %uD (2)", time_millis);
			return VOD_BAD_REQUEST;
		}

		if (time_millis < clip_time + *cur_duration)
		{
			break;
		}

		// get the clip start offset
		segmenter_get_start_offset(conf, segment_index, &clip_start_offset);

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

		// move to the next clip
		segment_index = clip_segment_limit;
	}

	// check bootstrap segments
	time_millis -= clip_time;

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
segmenter_get_start_end_ranges_gop(
	get_clip_ranges_params_t* params,
	get_clip_ranges_result_t* result)
{
	align_to_key_frames_context_t align_context;
	request_context_t* request_context = params->request_context;
	segmenter_conf_t* conf = params->conf;
	media_range_t* cur_clip_range;
	uint64_t clip_time;
	uint64_t start;
	uint64_t end;
	uint64_t* cur_clip_time;
	uint32_t* clip_durations = params->timing.durations;
	uint32_t* end_duration = clip_durations + params->timing.total_count;
	uint32_t* cur_duration;
	uint64_t time = params->time;
	uint32_t clip_duration;
	uint32_t clip_index;

	for (cur_duration = clip_durations, cur_clip_time = params->timing.times;
		;
		cur_duration++, cur_clip_time++)
	{
		if (cur_duration >= end_duration)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_gop: invalid time %uL (1)", time);
			return VOD_BAD_REQUEST;
		}

		clip_time = *cur_clip_time;
		if (time < clip_time)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_gop: invalid time %uL (2)", time);
			return VOD_BAD_REQUEST;
		}

		clip_duration = *cur_duration;
		if (time < clip_time + clip_duration)
		{
			break;
		}
	}

	clip_index = cur_duration - clip_durations;

	start = time - clip_time;
	if (start > conf->gop_look_behind)
	{
		start -= conf->gop_look_behind;
	}
	else
	{
		start = 0;
	}

	end = time - clip_time + conf->gop_look_ahead;
	if (end > clip_duration)
	{
		end = clip_duration;
	}

	if (params->key_frame_durations != NULL)
	{
		align_context.request_context = request_context;
		align_context.part = params->key_frame_durations;
		align_context.offset = params->timing.first_time + params->first_key_frame_offset - clip_time;
		align_context.cur_pos = align_context.part->first;
		if (start > 0)
		{
			start = segmenter_align_to_key_frames(&align_context, start, clip_duration);
		}
		end = segmenter_align_to_key_frames(&align_context, end, clip_duration);
	}

	// initialize the clip range
	cur_clip_range = vod_alloc(request_context->pool, sizeof(cur_clip_range[0]));
	if (cur_clip_range == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_start_end_ranges_gop: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_clip_range->timescale = 1000;
	cur_clip_range->start = start;
	cur_clip_range->end = end;
	cur_clip_range->original_clip_time = params->timing.original_times[clip_index];

	// initialize the result
	result->clip_time = clip_time;
	result->min_clip_index = result->max_clip_index = clip_index;
	result->clip_count = 1;
	result->clip_ranges = cur_clip_range;

	return VOD_OK;
}

vod_status_t
segmenter_get_start_end_ranges_no_discontinuity(
	get_clip_ranges_params_t* params,
	get_clip_ranges_result_t* result)
{
	align_to_key_frames_context_t align_context;
	request_context_t* request_context = params->request_context;
	uint64_t segment_base_time;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t clip_start_offset;
	uint64_t next_start_offset;
	uint64_t last_segment_end;
	uint64_t start;
	uint64_t end;
	media_range_t* cur_clip_range;
	uint32_t* clip_durations = params->timing.durations;
	uint32_t* end_duration = clip_durations + params->timing.total_count;
	uint32_t* cur_duration;
	uint32_t clip_initial_segment_index;
	uint32_t segment_count;
	uint32_t index;

	segment_base_time = params->timing.segment_base_time;
	if (segment_base_time == SEGMENT_BASE_TIME_RELATIVE)
	{
		segment_base_time = 0;
	}
	start_time = params->timing.first_time - segment_base_time;
	end_time = start_time + params->timing.total_duration;

	last_segment_end = params->last_segment_end;
	if (last_segment_end == 0)
	{
		last_segment_end = end_time;
	}

	// get the segment count
	segment_count = params->conf->get_segment_count(params->conf, end_time);
	if (segment_count == INVALID_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: segment count is invalid for total duration %uL", end_time);
		return VOD_BAD_DATA;
	}

	if (params->segment_index >= segment_count)
	{
		result->clip_count = 0;
		result->min_clip_index = 1;
		result->max_clip_index = 0;
		return VOD_OK;
	}

	// get the start / end offsets
	segmenter_get_start_end_offsets(
		params->conf,
		params->segment_index,
		&start,
		&end);

	if (end < start_time)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: segment end time %uL is less than sequence start time %uL",
			end, start_time);
		return VOD_BAD_REQUEST;
	}

	if (end > end_time && !params->allow_last_segment)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_start_end_ranges_no_discontinuity: request for the last segment in a live presentation (1)");
		return VOD_BAD_REQUEST;
	}

	if (start < start_time)
	{
		start = start_time;
	}

	if (params->key_frame_durations != NULL)
	{
		align_context.request_context = request_context;
		align_context.part = params->key_frame_durations;
		align_context.offset = start_time + params->first_key_frame_offset;
		align_context.cur_pos = align_context.part->first;
		start = segmenter_align_to_key_frames(&align_context, start, last_segment_end);
		end = segmenter_align_to_key_frames(&align_context, end, last_segment_end != ULLONG_MAX ? last_segment_end + 1 : last_segment_end);
		if (end > last_segment_end)
		{
			if (!params->allow_last_segment)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_start_end_ranges_no_discontinuity: request for the last segment in a live presentation (2)");
				return VOD_BAD_REQUEST;
			}

			end = last_segment_end;
		}
	}

	if (params->segment_index + 1 >= segment_count)
	{
		end = last_segment_end;
	}

	// find min/max clip indexes and initial sequence offset
	result->min_clip_index = INVALID_CLIP_INDEX;
	result->max_clip_index = params->timing.total_count - 1;

	for (cur_duration = clip_durations, clip_start_offset = start_time;
		cur_duration < end_duration; 
		cur_duration++, clip_start_offset = next_start_offset)
	{
		next_start_offset = clip_start_offset + *cur_duration;
		if (start >= next_start_offset)
		{
			continue;
		}

		if (start >= clip_start_offset)
		{
			result->min_clip_index = cur_duration - clip_durations;
			result->clip_time = clip_start_offset;
		}

		if (end <= next_start_offset)
		{
			result->max_clip_index = cur_duration - clip_durations;
			break;
		}
	}

	if (result->min_clip_index == INVALID_CLIP_INDEX)
	{
		result->clip_count = 0;
		result->min_clip_index = 1;
		result->max_clip_index = 0;
		return VOD_OK;
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
	start -= result->clip_time;
	end -= result->clip_time;
	for (index = result->min_clip_index;; index++, cur_clip_range++)
	{
		cur_clip_range->timescale = 1000;
		cur_clip_range->original_clip_time = params->timing.original_times[index];
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

	result->clip_time += segment_base_time;

	clip_initial_segment_index = segmenter_get_segment_index_no_discontinuity(
		params->conf,
		cur_clip_range->original_clip_time - segment_base_time);
	result->clip_relative_segment_index = params->segment_index - clip_initial_segment_index;
	
	return VOD_OK;
}

vod_status_t
segmenter_get_start_end_ranges_discontinuity(
	get_clip_ranges_params_t* params,
	get_clip_ranges_result_t* result)
{
	align_to_key_frames_context_t align_context;
	request_context_t* request_context = params->request_context;
	segmenter_conf_t* conf = params->conf;
	media_range_t* cur_clip_range;
	uint64_t* cur_clip_time;
	uint32_t* end_duration = params->timing.durations + params->timing.total_count;
	uint32_t* cur_duration;
	uint64_t clip_start_offset;
	uint64_t clip_time;
	uint64_t start;
	uint64_t end;
	uint32_t clip_initial_segment_index;
	uint32_t last_segment_limit;
	uint32_t cur_segment_limit;
	uint32_t segment_index = params->segment_index;
	uint32_t clip_index;
	uint32_t clip_duration;

	if (params->timing.segment_base_time == SEGMENT_BASE_TIME_RELATIVE)
	{
		// find the clip that contains segment_index
		last_segment_limit = params->initial_segment_index;
		for (cur_duration = params->timing.durations;; cur_duration++)
		{
			if (cur_duration >= end_duration)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_start_end_ranges_discontinuity: invalid segment index %uD (1)", segment_index);
				return VOD_BAD_REQUEST;
			}

			// get the clip start offset
			segmenter_get_start_offset(conf, last_segment_limit, &clip_start_offset);

			// get segment limit for the current clip
			clip_duration = *cur_duration;
			cur_segment_limit = conf->get_segment_count(conf, clip_start_offset + clip_duration);
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

			if (segment_index < cur_segment_limit)
			{
				// the segment index is within this clip, break
				break;
			}

			// move to the next clip
			last_segment_limit = cur_segment_limit;
		}

		if (segment_index < last_segment_limit)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_discontinuity: segment index %uD smaller than last segment index %uD",
				segment_index, last_segment_limit);
			return VOD_BAD_REQUEST;
		}

		// get the start/end times relative to clip_start_offset
		segmenter_get_start_end_offsets(
			conf,
			segment_index,
			&start,
			&end);

		clip_index = cur_duration - params->timing.durations;
		clip_time = params->timing.times[clip_index];
		clip_initial_segment_index = last_segment_limit;
	}
	else
	{
		// get the absolute start/end times
		segmenter_get_start_end_offsets(
			conf,
			segment_index,
			&start,
			&end);
		start += params->timing.segment_base_time;
		end += params->timing.segment_base_time;

		// find the clip that intersects start-end
		for (cur_duration = params->timing.durations, cur_clip_time = params->timing.times;
			; 
			cur_duration++, cur_clip_time++)
		{
			if (cur_duration >= end_duration)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_start_end_ranges_discontinuity: invalid segment index %uD (2)", segment_index);
				return VOD_BAD_REQUEST;
			}

			clip_time = *cur_clip_time;
			clip_duration = *cur_duration;
			if (end > clip_time && start < clip_time + clip_duration)
			{
				break;
			}
		}

		clip_index = cur_duration - params->timing.durations;
		clip_start_offset = clip_time;

		clip_initial_segment_index = segmenter_get_segment_index_no_discontinuity(
			conf,
			params->timing.original_times[clip_index] - params->timing.segment_base_time);

		cur_segment_limit = conf->get_segment_count(conf, clip_time + clip_duration - params->timing.segment_base_time);
		if (cur_segment_limit == INVALID_SEGMENT_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_discontinuity: segment count is invalid");
			return VOD_BAD_DATA;
		}
	}

	// get start / end position relative to the clip start
	if (start <= clip_start_offset)
	{
		start = 0;
	}
	else
	{
		start -= clip_start_offset;
	}

	if (segment_index + 1 >= cur_segment_limit)
	{
		if (end > clip_start_offset + clip_duration && 
			clip_index + 1 >= params->timing.total_count && 
			!params->allow_last_segment)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_start_end_ranges_discontinuity: request for the last segment in a live presentation (1)");
			return VOD_BAD_REQUEST;
		}
		end = clip_duration;
	}
	else
	{
		end -= clip_start_offset;
		if (end > clip_duration)
		{
			end = clip_duration;		// last segment in clip
		}
	}

	// align to key frames
	if (params->key_frame_durations != NULL)
	{
		align_context.request_context = request_context;
		align_context.part = params->key_frame_durations;
		align_context.offset = params->timing.first_time + params->first_key_frame_offset - clip_time;
		align_context.cur_pos = align_context.part->first;

		if (start > 0)
		{
			start = segmenter_align_to_key_frames(&align_context, start, clip_duration);
		}
		end = segmenter_align_to_key_frames(&align_context, end, clip_duration + 1);
		if (end > clip_duration)
		{
			if (clip_index + 1 >= params->timing.total_count && !params->allow_last_segment)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_start_end_ranges_discontinuity: request for the last segment in a live presentation (2)");
				return VOD_BAD_REQUEST;
			}

			end = clip_duration;
		}
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
	cur_clip_range->original_clip_time = params->timing.original_times[clip_index];

	// initialize the result
	result->clip_time = clip_time;
	result->min_clip_index = result->max_clip_index = clip_index;
	result->clip_count = 1;
	result->clip_ranges = cur_clip_range;
	result->clip_relative_segment_index = segment_index - clip_initial_segment_index;

	return VOD_OK;
}

static vod_status_t
segmenter_get_live_window_start_end(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set, 
	media_clip_timing_t* timing,
	live_window_start_end_t* result)
{
	align_to_key_frames_context_t align_context;
	media_sequence_t* sequence = media_set->sequences;
	int64_t live_window_duration = media_set->live_window_duration;
	uint64_t segment_base_time;
	uint64_t clip_end_time;
	uint64_t clip_time;
	uint64_t end_time;
	uint64_t start_time;
	uint32_t end_clip_offset;
	uint32_t end_clip_index;
	uint32_t start_clip_offset;
	uint32_t start_clip_index;
	uint32_t clip_duration;

	// validate the configuration for live
	if (conf->bootstrap_segments_count != 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_live_window_start_end: bootstrap segments not supported with live");
		return VOD_BAD_MAPPING;
	}

	if (conf->get_segment_count != segmenter_get_segment_count_last_short)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_live_window_start_end: segment_count_policy must be set to last_short in live");
		return VOD_BAD_MAPPING;
	}

	// find the end time
	if (live_window_duration <= 0)
	{
		// set the end time to the end of the last clip
		end_clip_index = timing->total_count - 1;
		end_clip_offset = timing->durations[end_clip_index];
		end_time = timing->times[end_clip_index] + end_clip_offset;

		live_window_duration = -live_window_duration;
		media_set->live_window_duration = live_window_duration;
	}
	else
	{
		// set the end time to current time
		end_time = (uint64_t)vod_time(request_context) * 1000;
		end_clip_index = UINT_MAX;
		end_clip_offset = 0;
	}

	if (!media_set->presentation_end && sequence->key_frame_durations != NULL)
	{
		// make sure end time does not exceed the last key frame time
		if ((uint64_t)sequence->last_key_frame_time < end_time)
		{
			end_time = sequence->last_key_frame_time;
			end_clip_index = UINT_MAX;
		}
	}

	if (end_clip_index == UINT_MAX)
	{
		// find the clip of the end time
		if (end_time <= timing->first_time)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_live_window_start_end: end time %uL smaller than first clip time %uL", end_time, timing->first_time);
			return VOD_BAD_MAPPING;
		}

		for (end_clip_index = timing->total_count - 1;; end_clip_index--)
		{
			clip_time = timing->times[end_clip_index];
			if (end_time > clip_time)
			{
				break;
			}
		}

		if (end_time < clip_time + timing->durations[end_clip_index])
		{
			end_clip_offset = end_time - clip_time;
		}
		else
		{
			end_clip_offset = timing->durations[end_clip_index];
			end_time = clip_time + end_clip_offset;
		}
	}

	// snap end to segment boundary
	if (!media_set->presentation_end ||
		end_clip_index < timing->total_count - 1 ||
		end_clip_offset < timing->durations[end_clip_index])
	{
		media_set->presentation_end = FALSE;

		// snap end to segment boundary
		if (timing->segment_base_time == SEGMENT_BASE_TIME_RELATIVE)
		{
			segment_base_time = timing->times[end_clip_index];
		}
		else
		{
			segment_base_time = timing->segment_base_time;
		}

		end_time = segment_base_time +
			((end_time - segment_base_time) / conf->segment_duration) * conf->segment_duration;

		if (end_time <= timing->times[end_clip_index])
		{
			// end slipped to the previous clip
			if (end_clip_index <= 0)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_live_window_start_end: empty window (2)");
				return VOD_BAD_MAPPING;
			}
			end_clip_index--;
			end_clip_offset = timing->durations[end_clip_index];
			end_time = timing->times[end_clip_index] + end_clip_offset;
		}
		else
		{
			// align to key frames
			if (sequence->key_frame_durations != NULL)
			{
				align_context.request_context = request_context;
				align_context.part = sequence->key_frame_durations;
				align_context.offset = timing->first_time + sequence->first_key_frame_offset;
				align_context.cur_pos = align_context.part->first;

				clip_end_time = timing->times[end_clip_index] + timing->durations[end_clip_index];
				end_time = segmenter_align_to_key_frames(&align_context, end_time, clip_end_time);
			}

			end_clip_offset = end_time - timing->times[end_clip_index];
		}
	}

	// find the start time
	if (live_window_duration == 0 ||
		(uint64_t)live_window_duration >= timing->total_duration ||
		(end_clip_index <= 0 && end_clip_offset < live_window_duration))
	{
		start_time = timing->times[0];
		start_clip_index = 0;
		start_clip_offset = 0;
	}
	else if (end_clip_offset >= live_window_duration)
	{
		start_time = end_time - live_window_duration;
		start_clip_index = end_clip_index;
		start_clip_offset = end_clip_offset - live_window_duration;
	}
	else
	{
		live_window_duration -= end_clip_offset;

		for (start_clip_index = end_clip_index - 1;; start_clip_index--)
		{
			clip_duration = timing->durations[start_clip_index];
			if (clip_duration >= live_window_duration)
			{
				start_clip_offset = clip_duration - live_window_duration;
				start_time = timing->times[start_clip_index] + start_clip_offset;
				break;
			}

			if (start_clip_index <= 0)
			{
				start_clip_offset = 0;
				start_time = timing->times[0];
				break;
			}

			live_window_duration -= clip_duration;
		}
	}

	if (!media_set->original_use_discontinuity ||
		start_clip_offset > 0 || 
		(start_clip_index <= 0 && timing->first_clip_start_offset > 0))
	{
		// snap start to segment boundary
		if (timing->segment_base_time == SEGMENT_BASE_TIME_RELATIVE)
		{
			segment_base_time = timing->times[start_clip_index];
		}
		else
		{
			segment_base_time = timing->segment_base_time;
		}

		start_time = segment_base_time +
			vod_div_ceil(start_time - segment_base_time, conf->segment_duration) * conf->segment_duration;

		// align start to key frames
		clip_end_time = timing->times[start_clip_index] + timing->durations[start_clip_index];

		if (sequence->key_frame_durations != NULL &&
			start_time > timing->times[start_clip_index])
		{
			align_context.request_context = request_context;
			align_context.part = sequence->key_frame_durations;
			align_context.offset = timing->first_time + sequence->first_key_frame_offset;
			align_context.cur_pos = align_context.part->first;

			start_time = segmenter_align_to_key_frames(&align_context, start_time, clip_end_time);
		}

		if (start_time >= clip_end_time)
		{
			// start slipped to the next clip
			start_clip_index++;
			if (start_clip_index > end_clip_index)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_live_window_start_end: empty window (1)");
				return VOD_BAD_MAPPING;
			}
			start_clip_offset = 0;
			start_time = timing->times[start_clip_index];
		}
		else
		{
			start_clip_offset = start_time - timing->times[start_clip_index];
		}
	}

	if (end_time <= start_time)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_live_window_start_end: empty window (3)");
		return VOD_BAD_MAPPING;
	}
	
	result->end_time = end_time;
	result->start_time = start_time;
	result->end_clip_offset = end_clip_offset;
	result->end_clip_index = end_clip_index;
	result->start_clip_offset = start_clip_offset;
	result->start_clip_index = start_clip_index;

	return VOD_OK;
}

vod_status_t
segmenter_get_live_window(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	bool_t parse_all_clips,
	get_clip_ranges_result_t* clip_ranges,
	uint32_t* base_clip_index)
{
	live_window_start_end_t window;
	media_clip_timing_t temp_timing;
	media_clip_timing_t* timing = &media_set->timing;
	media_sequence_t* sequence;
	vod_status_t rc;
	uint32_t* durations_end;
	uint32_t* durations_cur;
	uint32_t total_duration;
	uint32_t clip_initial_segment_index;
	uint32_t initial_segment_index;

	if (media_set->use_discontinuity)
	{
		rc = segmenter_get_live_window_start_end(
			request_context,
			conf,
			media_set,
			timing,
			&window);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// update the initial segment index
		if (timing->segment_base_time == SEGMENT_BASE_TIME_RELATIVE)
		{
			timing->first_segment_alignment_offset = window.start_clip_offset % conf->segment_duration;

			durations_end = timing->durations + window.start_clip_index;
			for (durations_cur = timing->durations; durations_cur < durations_end; durations_cur++)
			{
				media_set->initial_segment_index += vod_div_ceil(*durations_cur, conf->segment_duration);
			}

			media_set->initial_segment_clip_relative_index = window.start_clip_offset / conf->segment_duration;
			media_set->initial_segment_index += media_set->initial_segment_clip_relative_index;
		}
		else
		{
			clip_initial_segment_index = segmenter_get_segment_index_no_discontinuity(
				conf,
				timing->times[window.start_clip_index] - timing->segment_base_time);
			initial_segment_index = segmenter_get_segment_index_no_discontinuity(
				conf,
				window.start_time - timing->segment_base_time);
			media_set->initial_segment_clip_relative_index = initial_segment_index - clip_initial_segment_index;
		}
	}
	else
	{
		// no discontinuity - treat it like a single clip
		total_duration = media_set->timing.total_duration;
		temp_timing = media_set->timing;
		temp_timing.total_count = 1;
		temp_timing.durations = &total_duration;

		rc = segmenter_get_live_window_start_end(
			request_context,
			conf,
			media_set,
			&temp_timing,
			&window);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// calculate the clip indexes / offsets of start / end
		for (durations_cur = timing->durations;
			window.start_clip_offset >= *durations_cur;
			durations_cur++)
		{
			window.start_clip_offset -= *durations_cur;
			window.end_clip_offset -= *durations_cur;
			window.start_clip_index++;
		}

		window.end_clip_index = window.start_clip_index;

		for (;
			window.end_clip_offset > *durations_cur;
			durations_cur++)
		{
			window.end_clip_offset -= *durations_cur;
			window.end_clip_index++;
		}

		media_set->initial_segment_clip_relative_index = segmenter_get_segment_index_no_discontinuity(
			conf, 
			window.start_time - timing->segment_base_time);
	}

	// update the initial clip index
	if (media_set->use_discontinuity &&
		media_set->initial_clip_index != INVALID_CLIP_INDEX)
	{
		media_set->initial_clip_index += window.start_clip_index;
	}

	// trim the durations array
	// Note: start_clip_index and end_clip_index can be identical
	timing->durations[window.end_clip_index] = window.end_clip_offset;
	timing->durations += window.start_clip_index;
	timing->durations[0] -= window.start_clip_offset;

	timing->total_count = window.end_clip_index + 1 - window.start_clip_index;

	// recalculate the total duration
	timing->total_duration = 0;
	durations_end = timing->durations + timing->total_count;
	for (durations_cur = timing->durations; durations_cur < durations_end; durations_cur++)
	{
		timing->total_duration += *durations_cur;
	}

	// adjust the first key frame offsets
	for (sequence = media_set->sequences; sequence < media_set->sequences_end; sequence++)
	{
		sequence->first_key_frame_offset -= window.start_time - timing->first_time;
	}

	// trim the clip times array
	timing->times += window.start_clip_index;
	timing->original_first_time = timing->times[0];
	if (window.start_clip_index <= 0)
	{
		timing->original_first_time -= timing->first_clip_start_offset;
	}
	timing->times[0] = window.start_time;
	timing->first_time = window.start_time;

	if (parse_all_clips)
	{
		// parse all clips
		if (timing->total_count > MAX_CLIPS_PER_REQUEST)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"segmenter_get_live_window: clip count %uD exceeds the limit per request", timing->total_count);
			return VOD_BAD_REQUEST;
		}

		clip_ranges->clip_count = timing->total_count;
		clip_ranges->min_clip_index = 0;
	}
	else
	{
		// parse only the last clip in each sequence, assume subsequent clips have the same media info
		clip_ranges->clip_count = 1;
		clip_ranges->min_clip_index = window.end_clip_index - window.start_clip_index;
	}

	clip_ranges->max_clip_index = window.end_clip_index - window.start_clip_index;
	clip_ranges->clip_time = timing->first_time;

	*base_clip_index += window.start_clip_index;

	return VOD_OK;
}

static vod_inline void
segmenter_get_segment_durations_add(
	segmenter_get_segment_durations_context_t* context,
	uint32_t segment_duration,
	uint32_t segment_count)
{
	segment_duration_item_t* cur_item;
	uint64_t next_aligned_time;
	uint64_t cur_time;

	// update cur time
	cur_time = context->cur_time;
	context->cur_time = cur_time + (uint64_t)segment_duration * segment_count;

	// align to key frames
	if (context->align.part != NULL)
	{
		next_aligned_time = segmenter_align_to_key_frames(
			&context->align, 
			context->cur_time, 
			context->clip_end_time);
		cur_time = context->aligned_time;
		segment_duration = next_aligned_time - cur_time;
		context->aligned_time = next_aligned_time;
	}

	// add the duration
	cur_item = context->cur_item;
	if (cur_item < context->result->items || 
		segment_duration != cur_item->duration || 
		context->discontinuity)
	{
		cur_item++;
		cur_item->segment_index = context->segment_index;
		cur_item->time = cur_time;
		cur_item->duration = segment_duration;
		cur_item->repeat_count = segment_count;
		cur_item->discontinuity = context->discontinuity;
		context->cur_item = cur_item;
		context->discontinuity = FALSE;
	}
	else
	{
		cur_item->repeat_count += segment_count;
	}

	// update segment count / index
	context->result->segment_count += segment_count;
	context->segment_index += segment_count;
}

static vod_status_t
segmenter_get_segment_durations_estimate_internal(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_clip_timing_t* timing,
	uint64_t cur_clip_duration,
	uint32_t initial_segment_index,
	media_sequence_t* sequence,
	segment_durations_t* result)
{
	segmenter_get_segment_durations_context_t context;
	uint32_t* end_duration = timing->durations + timing->total_count;
	uint32_t* cur_duration = timing->durations;
	uint64_t segment_start;
	uint64_t segment_end;
	uint32_t bootstrap_segment_limit;
	uint32_t clip_segment_limit;
	uint32_t new_segment_index;
	uint32_t alignment_offset;
	uint32_t segment_duration;
	uint32_t segment_count;
	uint32_t alloc_count;
	uint64_t* cur_clip_time = timing->times;

	// initialize the align context
	if (sequence->key_frame_durations != NULL)
	{
		context.align.request_context = request_context;
		context.align.part = sequence->key_frame_durations;
		context.align.offset = timing->first_time + sequence->first_key_frame_offset;
		context.align.cur_pos = context.align.part->first;
		alloc_count = conf->bootstrap_segments_count + 2 * timing->total_count +
			vod_div_ceil(result->duration, conf->segment_duration);
	}
	else
	{
		vod_memzero(&context.align, sizeof(context.align));
		alloc_count = conf->bootstrap_segments_count + 3 * timing->total_count;
	}

	// allocate the result buffer
	result->items = vod_alloc(request_context->pool, sizeof(result->items[0]) * alloc_count);
	if (result->items == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_segment_durations_estimate_internal: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->segment_count = 0;

	// initialize the context
	context.result = result;
	context.cur_item = result->items - 1;
	context.discontinuity = FALSE;
	context.segment_index = initial_segment_index;

	alignment_offset = timing->first_segment_alignment_offset;

	for (;;)
	{
		// update time fields
		context.cur_time = *cur_clip_time - alignment_offset;
		context.aligned_time = *cur_clip_time;
		context.clip_end_time = context.aligned_time + cur_clip_duration;

		if (timing->segment_base_time != SEGMENT_BASE_TIME_RELATIVE)
		{
			// get the segment index
			new_segment_index = segmenter_get_segment_index_no_discontinuity(
				conf,
				*cur_clip_time - timing->segment_base_time);

			if (new_segment_index < context.segment_index)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_segment_durations_estimate_internal: timing gap smaller than segment duration, when using absolute segment base time");
				return VOD_BAD_MAPPING;
			}

			context.segment_index = new_segment_index;

			// get the last segment index for this clip
			clip_segment_limit = conf->get_segment_count(conf, context.clip_end_time - timing->segment_base_time);
			if (clip_segment_limit == INVALID_SEGMENT_COUNT)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_segment_durations_estimate_internal: segment count is invalid");
				return VOD_BAD_DATA;
			}

			clip_segment_limit = clip_segment_limit > context.segment_index ?
				clip_segment_limit - 1 :
				context.segment_index;

			if (context.segment_index < clip_segment_limit)
			{
				// align-to-boundary segment
				segmenter_get_end_offset(conf, context.segment_index, &segment_end);

				segment_duration = timing->segment_base_time + segment_end - *cur_clip_time;

				segmenter_get_segment_durations_add(
					&context,
					segment_duration,
					1);
			}
		}
		else
		{
			// find the clip start offset
			segmenter_get_start_offset(conf, context.segment_index, &segment_start);

			// get segment limit for the current clip
			clip_segment_limit = conf->get_segment_count(conf, segment_start + alignment_offset + cur_clip_duration);
			if (clip_segment_limit == INVALID_SEGMENT_COUNT)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"segmenter_get_segment_durations_estimate_internal: segment count is invalid");
				return VOD_BAD_DATA;
			}

			clip_segment_limit = clip_segment_limit > context.segment_index ?
				clip_segment_limit - 1 :
				context.segment_index;
		}

		cur_clip_time++;

		// bootstrap segments
		bootstrap_segment_limit = vod_min(conf->bootstrap_segments_count, clip_segment_limit);
		while (context.segment_index < bootstrap_segment_limit)
		{
			segment_duration = conf->bootstrap_segments_durations[context.segment_index];

			segmenter_get_segment_durations_add(
				&context,
				segment_duration,
				1);
		}

		// regular segments
		// Note: when key frame alignment is used, must add the segments one by one since they can have different durations
		segment_duration = conf->segment_duration;
		segment_count = sequence->key_frame_durations != NULL ? 1 : clip_segment_limit - context.segment_index;
		while (context.segment_index < clip_segment_limit)
		{
			segmenter_get_segment_durations_add(
				&context,
				segment_duration,
				segment_count);
		}

		// last segment
		segmenter_get_segment_durations_add(
			&context,
			context.clip_end_time - context.cur_time,
			1);

		// there may be zero duration segments at the end due to key frame alignment, if so, strip them off
		if (context.cur_item->duration == 0)
		{
			context.result->segment_count -= context.cur_item->repeat_count;
			context.cur_item--;
		}

		// move to the next clip
		cur_duration++;
		if (cur_duration >= end_duration)
		{
			break;
		}

		cur_clip_duration = *cur_duration;

		context.discontinuity = TRUE;
		alignment_offset = 0;
	}

	// finalize the result
	if (result->segment_count > MAX_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_estimate_internal: segment count %uD is invalid", result->segment_count);
		return VOD_BAD_MAPPING;
	}

	result->item_count = context.cur_item + 1 - result->items;
	result->timescale = 1000;
	result->discontinuities = timing->total_count - 1;

	return VOD_OK;
}

uint64_t
segmenter_get_total_duration(
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	media_sequence_t* sequences_end,
	uint32_t media_type)
{
	media_track_t** ref_track;
	media_sequence_t* cur_sequence;
	uint32_t cur_media_type;
	uint32_t min_media_type;
	uint32_t max_media_type;
	uint64_t result = 0;

	if (media_type != MEDIA_TYPE_NONE)
	{
		switch (conf->manifest_duration_policy)
		{
		case MDP_MAX:
			for (cur_sequence = sequence; cur_sequence < sequences_end; cur_sequence++)
			{
				ref_track = cur_sequence->filtered_clips[0].ref_track;
				if (ref_track[media_type] == NULL)
				{
					continue;
				}

				if (ref_track[media_type]->media_info.duration_millis > result)
				{
					result = ref_track[media_type]->media_info.duration_millis;
				}
			}
			break;

		case MDP_MIN:
			for (cur_sequence = sequence; cur_sequence < sequences_end; cur_sequence++)
			{
				ref_track = cur_sequence->filtered_clips[0].ref_track;
				if (ref_track[media_type] == NULL)
				{
					continue;
				}

				if (ref_track[media_type]->media_info.duration_millis > 0 &&
					(result == 0 || ref_track[media_type]->media_info.duration_millis < result))
				{
					result = ref_track[media_type]->media_info.duration_millis;
				}
			}
			break;
		}

		return result;
	}

	// ignore subtitles duration if there are audio/video streams
	if (media_set->track_count[MEDIA_TYPE_VIDEO] + media_set->track_count[MEDIA_TYPE_AUDIO] > 0)
	{
		min_media_type = 0;
		max_media_type = MEDIA_TYPE_SUBTITLE;
	}
	else
	{
		min_media_type = MEDIA_TYPE_SUBTITLE;
		max_media_type = MEDIA_TYPE_COUNT;
	}

	switch (conf->manifest_duration_policy)
	{
	case MDP_MAX:
		for (cur_sequence = sequence; cur_sequence < sequences_end; cur_sequence++)
		{
			ref_track = cur_sequence->filtered_clips[0].ref_track;

			for (cur_media_type = min_media_type; cur_media_type < max_media_type; cur_media_type++)
			{
				if (ref_track[cur_media_type] == NULL)
				{
					continue;
				}

				if (ref_track[cur_media_type]->media_info.duration_millis > result)
				{
					result = ref_track[cur_media_type]->media_info.duration_millis;
				}
			}
		}
		break;

	case MDP_MIN:
		for (cur_sequence = sequence; cur_sequence < sequences_end; cur_sequence++)
		{
			ref_track = cur_sequence->filtered_clips[0].ref_track;

			for (cur_media_type = min_media_type; cur_media_type < max_media_type; cur_media_type++)
			{
				if (ref_track[cur_media_type] == NULL)
				{
					continue;
				}

				if (ref_track[cur_media_type]->media_info.duration_millis > 0 && 
					(result == 0 || ref_track[cur_media_type]->media_info.duration_millis < result))
				{
					result = ref_track[cur_media_type]->media_info.duration_millis;
				}
			}
		}
		break;
	}

	return result;
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
	media_clip_timing_t timing;
	media_sequence_t* sequences_end;
	uint64_t duration_millis;

	if (sequence != NULL)
	{
		sequences_end = sequence + 1;
	}
	else
	{
		sequence = media_set->sequences;
		sequences_end = media_set->sequences_end;
	}

	if (media_set->timing.durations != NULL)
	{
		duration_millis = media_set->timing.total_duration;

		if (media_set->use_discontinuity)
		{
			result->start_time = media_set->timing.first_time;
			result->end_time = 
				media_set->timing.times[media_set->timing.total_count - 1] +
				media_set->timing.durations[media_set->timing.total_count - 1];
			result->duration = duration_millis;

			return segmenter_get_segment_durations_estimate_internal(
				request_context,
				conf,
				&media_set->timing,
				media_set->timing.durations[0],
				media_set->initial_segment_index,
				sequence,
				result);
		}

		// no discontinuity - treat it like a single clip
	}
	else
	{
		duration_millis = segmenter_get_total_duration(
			conf,
			media_set,
			sequence,
			sequences_end,
			media_type);
	}

	result->start_time = media_set->timing.first_time;
	result->end_time = media_set->timing.first_time + duration_millis;
	result->duration = duration_millis;

	vod_memzero(&timing, sizeof(timing));
	timing.total_count = 1;
	timing.segment_base_time = media_set->timing.segment_base_time;
	timing.first_time = media_set->timing.first_time;
	timing.times = &timing.first_time;

	return segmenter_get_segment_durations_estimate_internal(
		request_context,
		conf,
		&timing,
		duration_millis,
		media_set->initial_segment_index,
		sequence,
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
	media_track_t* ref_track = NULL;
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

	if (media_set->timing.durations != NULL)
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
				(cur_track->media_info.media_type < main_track->media_info.media_type))
			{
				main_track = cur_track;
			}

			if (ref_track == NULL)
			{
				ref_track = cur_track;
				duration_millis = cur_track->media_info.duration_millis;
			}
			else
			{
				switch (conf->manifest_duration_policy)
				{
				case MDP_MAX:
					if (cur_track->media_info.duration_millis > duration_millis)
					{
						ref_track = cur_track;
						duration_millis = cur_track->media_info.duration_millis;
					}
					break;

				case MDP_MIN:
					if (cur_track->media_info.duration_millis > 0 &&
						(duration_millis == 0 || cur_track->media_info.duration_millis < duration_millis))
					{
						ref_track = cur_track;
						duration_millis = cur_track->media_info.duration_millis;
					}
					break;
				}
			}
		}
	}

	if (main_track == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: didn't get any tracks");
		return VOD_UNEXPECTED;
	}

	// if the main track is not audio/video, or main track is audio and requires filtering, fall back to estimate
	switch (main_track->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		break;

	case MEDIA_TYPE_AUDIO:
		if (!media_set->audio_filtering_needed)
		{
			break;
		}
		// fall through

	default:
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

	// Note: assuming a single frame list part
	cur_item = result->items - 1;
	last_frame = main_track->frames.last_frame;
	cur_frame = main_track->frames.first_frame;

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
					cur_item->time = segment_start;
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
				cur_item->time = segment_start;
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

		total_duration = rescale_time(ref_track->media_info.duration, ref_track->media_info.timescale, result->timescale);

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
				cur_item->time = segment_start;
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
			cur_item->time = segment_start;
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
	result->duration = duration_millis;

	return VOD_OK;
}
