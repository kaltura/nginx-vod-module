#include "webvtt_builder.h"

// constants
#define WEBVTT_TIMESTAMP_FORMAT "%02uD:%02uD:%02uD.%03uD"
#define WEBVTT_TIMESTAMP_DELIM " --> "
#define WEBVTT_TIMESTAMP_MAX_SIZE (VOD_INT32_LEN + sizeof(":00:00.000") - 1)
#define WEBVTT_CUE_TIMINGS_MAX_SIZE (WEBVTT_TIMESTAMP_MAX_SIZE * 2 + sizeof(WEBVTT_TIMESTAMP_DELIM) - 1)

static u_char*
webvtt_builder_write_timestamp(u_char* p, uint64_t timestamp)
{
	return vod_sprintf(p, WEBVTT_TIMESTAMP_FORMAT,
		(uint32_t)(timestamp / 3600000),
		(uint32_t)((timestamp / 60000) % 60),
		(uint32_t)((timestamp / 1000) % 60),
		(uint32_t)(timestamp % 1000));
}

vod_status_t
webvtt_builder_build(
	request_context_t* request_context,
	media_set_t* media_set,
	bool_t clip_relative_timestamps,
	vod_str_t* result)
{
	frame_list_part_t* part;
	media_track_t* cur_track;
	media_track_t* first_track = media_set->filtered_tracks;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t start_time;
	uint32_t id_size;
	size_t result_size;
	u_char* src;
	u_char* p;

	// get the result size
	result_size = first_track->media_info.extra_data.len;
	for (cur_track = first_track; cur_track < media_set->filtered_tracks_end; cur_track++)
	{
		result_size += cur_track->total_frames_size + WEBVTT_CUE_TIMINGS_MAX_SIZE * cur_track->frame_count;
	}

	// allocate the buffer
	p = vod_alloc(request_context->pool, result_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_builder_build: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->data = p;

	// webvtt header
	p = vod_copy(p, first_track->media_info.extra_data.data, first_track->media_info.extra_data.len);

	for (cur_track = first_track; cur_track < media_set->filtered_tracks_end; cur_track++)
	{
		start_time = cur_track->first_frame_time_offset;
		if (!clip_relative_timestamps)
		{
			start_time += cur_track->clip_start_time;
		}
		part = &cur_track->frames;
		last_frame = part->last_frame;
		for (cur_frame = part->first_frame;; cur_frame++)
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

			src = (u_char*)(uintptr_t)cur_frame->offset;

			// cue identifier
			id_size = cur_frame->key_frame;
			p = vod_copy(p, src, id_size);
			src += id_size;

			// cue timings
			p = webvtt_builder_write_timestamp(p, start_time);
			p = vod_copy(p, WEBVTT_TIMESTAMP_DELIM, sizeof(WEBVTT_TIMESTAMP_DELIM) - 1);
			p = webvtt_builder_write_timestamp(p, start_time + cur_frame->pts_delay);
			start_time += cur_frame->duration;

			// cue settings list + cue payload
			p = vod_copy(p, src, cur_frame->size - id_size);
		}
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"webvtt_builder_build: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}
