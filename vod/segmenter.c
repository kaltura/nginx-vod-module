#include "segmenter.h"

#define MAX_SEGMENT_COUNT (100000)

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
segmenter_get_segment_count_last_short(segmenter_conf_t* conf, uint32_t duration_millis)
{
	uint32_t result;

	if (duration_millis == 0)
	{
		return 0;
	}

	if (duration_millis > conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;
		result = conf->bootstrap_segments_count + vod_div_ceil(duration_millis, conf->segment_duration);
		if (result > MAX_SEGMENT_COUNT)
		{
			return INVALID_SEGMENT_COUNT;
		}
	}
	else
	{
		for (result = 1; result < conf->bootstrap_segments_count && duration_millis > conf->bootstrap_segments_start[result]; result++);
	}

	return result;
}

uint32_t
segmenter_get_segment_count_last_long(segmenter_conf_t* conf, uint32_t duration_millis)
{
	uint32_t result;

	if (duration_millis == 0)
	{
		return 0;
	}

	if (duration_millis >= conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;
		result = conf->bootstrap_segments_count + duration_millis / conf->segment_duration;
		if (result < 1)
		{
			result = 1;
		}
		else if (result > MAX_SEGMENT_COUNT)
		{
			return INVALID_SEGMENT_COUNT;
		}
	}
	else
	{
		for (result = 1; result < conf->bootstrap_segments_count && duration_millis >= conf->bootstrap_segments_end[result]; result++);
	}

	return result;
}

uint32_t
segmenter_get_segment_count_last_rounded(segmenter_conf_t* conf, uint32_t duration_millis)
{
	uint32_t result;

	if (duration_millis == 0)
	{
		return 0;
	}

	if (duration_millis >= conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;
		result = conf->bootstrap_segments_count + (duration_millis + conf->segment_duration / 2) / conf->segment_duration;
		if (result < 1)
		{
			result = 1;
		}
		else if (result > MAX_SEGMENT_COUNT)
		{
			return INVALID_SEGMENT_COUNT;
		}
	}
	else
	{
		for (result = 1; result < conf->bootstrap_segments_count && duration_millis >= conf->bootstrap_segments_mid[result]; result++);
	}

	return result;
}

void
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
segmenter_get_segment_index(segmenter_conf_t* conf, uint32_t time_millis)
{
	uint32_t* cur_pos;
	uint32_t result;

	if (time_millis >= conf->bootstrap_segments_total_duration)
	{
		return conf->bootstrap_segments_count + 
			(time_millis - conf->bootstrap_segments_total_duration) / conf->segment_duration;
	}

	result = 0;
	for (cur_pos = conf->bootstrap_segments_end; time_millis >= *cur_pos; cur_pos++)
	{
		result++;
	}
	return result;
}

vod_status_t
segmenter_get_segment_durations_estimate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	mpeg_stream_metadata_t** streams,
	uint32_t stream_count,
	segment_durations_t* result)
{
	mpeg_stream_metadata_t** cur_stream;
	mpeg_stream_metadata_t** last_stream = streams + stream_count;
	segment_duration_item_t* cur_item;
	uint32_t* durations_end;
	uint32_t* segment_duration;
	uint32_t segment_index = 0;
	uint32_t cur_duration;
	uint32_t total_duration = 0;

	// get the maximum duration
	result->duration_millis = 0;
	for (cur_stream = streams; cur_stream < last_stream; cur_stream++)
	{
		if (*cur_stream == NULL)
		{
			continue;
		}

		if ((*cur_stream)->media_info.duration_millis > result->duration_millis)
		{
			result->duration_millis = (*cur_stream)->media_info.duration_millis;
		}
	}

	// get the segment count
	result->segment_count = conf->get_segment_count(conf, result->duration_millis);
	if (result->segment_count == INVALID_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_estimate: segment count is invalid");
		return VOD_BAD_DATA;
	}

	// allocate the result buffer
	result->items = vod_alloc(request_context->pool, sizeof(*result->items) * (conf->bootstrap_segments_count + 2));
	if (result->items == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"segmenter_get_segment_durations_estimate: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->timescale = 1000;

	// bootstrap segments
	cur_item = result->items - 1;

	durations_end = conf->bootstrap_segments_durations + conf->bootstrap_segments_count;
	for (segment_duration = conf->bootstrap_segments_durations; 
		segment_duration < durations_end && segment_index + 1 < result->segment_count; 
		segment_duration++)
	{
		cur_duration = *segment_duration;	
		if (cur_item < result->items || cur_duration != cur_item->duration)
		{
			cur_item++;
			cur_item->repeat_count = 0;
			cur_item->segment_index = segment_index;
			cur_item->duration = cur_duration;
		}
		cur_item->repeat_count++;

		total_duration += cur_duration;
		segment_index++;
	}

	// remaining segments
	if (segment_index + 1 < result->segment_count)
	{
		cur_duration = conf->segment_duration;
		if (cur_item < result->items || cur_duration != cur_item->duration)
		{
			cur_item++;
			cur_item->repeat_count = 0;
			cur_item->segment_index = segment_index;
			cur_item->duration = cur_duration;
		}
		cur_item->repeat_count += result->segment_count - segment_index - 1;

		total_duration += cur_duration * (result->segment_count - segment_index - 1);
		segment_index = result->segment_count - 1;
	}

	// last segment
	if (segment_index < result->segment_count && total_duration < result->duration_millis)
	{
		cur_duration = result->duration_millis - total_duration;
		if (cur_item < result->items || cur_duration != cur_item->duration)
		{
			cur_item++;
			cur_item->repeat_count = 0;
			cur_item->segment_index = segment_index;
			cur_item->duration = cur_duration;
		}
		cur_item->repeat_count++;
	}

	result->item_count = cur_item + 1 - result->items;

	return VOD_OK;
}

vod_status_t 
segmenter_get_segment_durations_accurate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	mpeg_stream_metadata_t** streams,
	uint32_t stream_count,
	segment_durations_t* result)
{
	segmenter_boundary_iterator_context_t boundary_iterator;
	mpeg_stream_metadata_t** cur_stream;
	mpeg_stream_metadata_t** last_stream = streams + stream_count;
	mpeg_stream_metadata_t* main_stream = NULL;
	mpeg_stream_metadata_t* longest_stream = NULL;
	segment_duration_item_t* cur_item;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	uint64_t total_duration;
	uint32_t segment_index = 0;
	uint64_t accum_duration = 0;
	uint64_t segment_start = 0;
	uint64_t segment_limit_millis;
	uint64_t segment_limit;
	uint64_t cur_duration;
	bool_t align_to_key_frames;

	// get the maximum duration and main stream (=first non null stream)
	result->duration_millis = 0;
	for (cur_stream = streams; cur_stream < last_stream; cur_stream++)
	{
		if (*cur_stream == NULL)
		{
			continue;
		}

		if (main_stream == NULL)
		{
			main_stream = *cur_stream;
		}

		if ((*cur_stream)->media_info.duration_millis > result->duration_millis)
		{
			longest_stream = *cur_stream;
			result->duration_millis = (*cur_stream)->media_info.duration_millis;
		}
	}

	if (main_stream == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: didnt get any streams");
		return VOD_UNEXPECTED;
	}

	// get the segment count
	result->segment_count = conf->get_segment_count(conf, result->duration_millis);
	if (result->segment_count == INVALID_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"segmenter_get_segment_durations_accurate: segment count is invalid");
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

	result->timescale = main_stream->media_info.timescale;

	cur_item = result->items - 1;
	last_frame = main_stream->frames + main_stream->frame_count;
	cur_frame = main_stream->frames;

	align_to_key_frames = conf->align_to_key_frames && main_stream->media_info.media_type == MEDIA_TYPE_VIDEO;

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
	
	// in case the main video stream is shorter than the audio stream, add the estimated durations of the remaining audio-only segments
	if (main_stream->media_info.duration_millis < result->duration_millis && 
		!align_to_key_frames)
	{
		segmenter_boundary_iterator_init(&boundary_iterator, conf, result->segment_count);
		segmenter_boundary_iterator_skip(&boundary_iterator, segment_index);

		total_duration = rescale_time(longest_stream->media_info.duration, longest_stream->media_info.timescale, result->timescale);

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

	return VOD_OK;
}

void 
segmenter_boundary_iterator_init(segmenter_boundary_iterator_context_t* context, segmenter_conf_t* conf, uint32_t segment_count)
{
	context->conf = conf;
	context->segment_index = 0;
	context->segment_count = segment_count;
	context->last_boundary = context->conf->bootstrap_segments_total_duration;
}

uint32_t
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

void
segmenter_boundary_iterator_skip(segmenter_boundary_iterator_context_t* context, uint32_t count)
{
	context->segment_index = vod_min(context->segment_index + count, context->segment_count);

	if (context->segment_index > context->conf->bootstrap_segments_count)
	{
		context->last_boundary = context->conf->bootstrap_segments_total_duration + 
			(context->segment_index - context->conf->bootstrap_segments_count) * context->conf->segment_duration;
	}
}
