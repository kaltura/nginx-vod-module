#include "segmenter.h"

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

	if (duration_millis >= conf->bootstrap_segments_total_duration)
	{
		duration_millis -= conf->bootstrap_segments_total_duration;
		result = conf->bootstrap_segments_count + DIV_CEIL(duration_millis, conf->segment_duration);
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
	mpeg_stream_metadata_t* cur_stream,
	uint32_t segment_count,
	segment_durations_t* result)
{
	segment_duration_item_t* cur_item;
	uint32_t* durations_end;
	uint32_t* segment_duration;
	uint32_t segment_index = 0;
	uint32_t cur_duration;
	uint32_t total_duration = 0;

	result->items = vod_alloc(request_context->pool, sizeof(*result->items) * (conf->bootstrap_segments_count + 2));
	if (result->items == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	result->timescale = 1000;

	cur_item = result->items - 1;

	durations_end = conf->bootstrap_segments_durations + conf->bootstrap_segments_count;
	for (segment_duration = conf->bootstrap_segments_durations; segment_duration < durations_end && segment_index + 1 < segment_count; segment_duration++)
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

	if (segment_index + 1 < segment_count)
	{
		cur_duration = conf->segment_duration;
		if (cur_item < result->items || cur_duration != cur_item->duration)
		{
			cur_item++;
			cur_item->repeat_count = 0;
			cur_item->segment_index = segment_index;
			cur_item->duration = cur_duration;
		}
		cur_item->repeat_count += segment_count - segment_index - 1;

		total_duration += cur_duration * (segment_count - segment_index - 1);
		segment_index = segment_count - 1;
	}

	if (segment_index < segment_count && total_duration < cur_stream->media_info.duration_millis)
	{
		cur_duration = cur_stream->media_info.duration_millis - total_duration;
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
	mpeg_stream_metadata_t* cur_stream, 
	uint32_t segment_count, 
	segment_durations_t* result)
{
	segment_duration_item_t* cur_item;
	input_frame_t* last_frame = cur_stream->frames + cur_stream->frame_count;
	input_frame_t* cur_frame = cur_stream->frames;
	uint32_t segment_index = 0;
	uint64_t accum_duration = 0;
	uint64_t segment_start = 0;
	uint64_t segment_limit_ts;
	uint64_t segment_limit;
	uint64_t cur_duration;
	bool_t align_to_key_frames;

	result->items = vod_alloc(request_context->pool, sizeof(*result->items) * segment_count);
	if (result->items == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	result->timescale = cur_stream->media_info.timescale;

	cur_item = result->items - 1;

	align_to_key_frames = conf->align_to_key_frames && cur_stream->media_info.media_type == MEDIA_TYPE_VIDEO;

	if (conf->bootstrap_segments_count > 0)
	{
		segment_limit = rescale_time(conf->bootstrap_segments_end[0], 1000, cur_stream->media_info.timescale);

		for (; cur_frame < last_frame; cur_frame++)
		{
			while (accum_duration >= segment_limit && segment_index + 1 < segment_count && 
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
				segment_limit = rescale_time(conf->bootstrap_segments_end[segment_index], 1000, cur_stream->media_info.timescale);
			}
			accum_duration += cur_frame->duration;
		}
	}

post_bootstrap:

	segment_limit_ts = conf->bootstrap_segments_total_duration + conf->segment_duration;
	segment_limit = rescale_time(segment_limit_ts, 1000, cur_stream->media_info.timescale);

	for (; cur_frame < last_frame; cur_frame++)
	{
		while (accum_duration >= segment_limit && segment_index + 1 < segment_count &&
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
			segment_limit_ts = conf->bootstrap_segments_total_duration + (uint64_t)conf->segment_duration * (segment_index - conf->bootstrap_segments_count + 1);
			segment_limit = rescale_time(segment_limit_ts, 1000, cur_stream->media_info.timescale);
		}
		accum_duration += cur_frame->duration;
	}

	while (segment_index < segment_count)
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
