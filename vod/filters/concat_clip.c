#include "concat_clip.h"
#include "../media_set_parser.h"
#include "../parse_utils.h"

// constants
#define MAX_CONCAT_ELEMENTS (65536)

// typedefs
typedef struct {
	media_clip_t base;
} media_clip_concat_t;

// enums
enum {
	CONCAT_PARAM_BASE_PATH,
	CONCAT_PARAM_PATHS,
	CONCAT_PARAM_CLIP_IDS,
	CONCAT_PARAM_DURATIONS,
	CONCAT_PARAM_OFFSET,
	CONCAT_PARAM_TRACKS,
	CONCAT_PARAM_NOTIFICATIONS,

	CONCAT_PARAM_COUNT
};

// constants
static json_object_key_def_t concat_clip_params[] = {
	{ vod_string("basePath"),		VOD_JSON_STRING,	CONCAT_PARAM_BASE_PATH },
	{ vod_string("paths"),			VOD_JSON_ARRAY,		CONCAT_PARAM_PATHS },
	{ vod_string("clipIds"),		VOD_JSON_ARRAY,		CONCAT_PARAM_CLIP_IDS },
	{ vod_string("durations"),		VOD_JSON_ARRAY,		CONCAT_PARAM_DURATIONS },
	{ vod_string("offset"),			VOD_JSON_INT,		CONCAT_PARAM_OFFSET },
	{ vod_string("tracks"),			VOD_JSON_STRING,	CONCAT_PARAM_TRACKS },
	{ vod_string("notifications"),	VOD_JSON_ARRAY,		CONCAT_PARAM_NOTIFICATIONS },
	{ vod_null_string, 0, 0 }
};

// globals
static vod_hash_t concat_clip_hash;

vod_status_t
concat_clip_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	vod_array_part_t* first_part = NULL;
	vod_array_part_t* part;
	media_clip_source_t* sources_list_head;
	media_clip_source_t* cur_source;
	media_clip_source_t* sources_end;
	media_clip_source_t* sources;
	media_clip_concat_t* clip;
	vod_json_value_t* params[CONCAT_PARAM_COUNT];
	vod_json_array_t* durations;
	vod_json_array_t* paths;
	media_range_t* range_cur;
	media_range_t* range;
	vod_str_t* src_str;
	vod_str_t base_path;
	vod_str_t dest_str;
	u_char* end_pos;
	int64_t* first_duration = NULL;
	int64_t* cur_duration;
	int64_t cur_duration_value;
	int64_t clip_time;
	uint64_t original_clip_time;
	uint64_t start;
	uint64_t end;
	track_mask_t tracks_mask[MEDIA_TYPE_COUNT];
	uint32_t min_index;
	uint32_t max_index;
	uint32_t clip_count;
	int32_t start_offset = 0;
	int32_t next_offset;
	int32_t offset;
	uint32_t i;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"concat_clip_parse: started");

	// get the required fields
	vod_memzero(params, sizeof(params));
	vod_json_get_object_values(
		element,
		&concat_clip_hash,
		params);

	// validate the paths and durations arrays
	if (params[CONCAT_PARAM_PATHS] != NULL)
	{
		paths = &params[CONCAT_PARAM_PATHS]->v.arr;
		sources_list_head = context->sources_head;
	}
	else if (params[CONCAT_PARAM_CLIP_IDS] != NULL)
	{
		paths = &params[CONCAT_PARAM_CLIP_IDS]->v.arr;
		sources_list_head = context->mapped_sources_head;
	}
	else
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_clip_parse: must specify either \"paths\" or \"clipId\" for concat");
		return VOD_BAD_MAPPING;
	}

	if (paths->type != VOD_JSON_STRING)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_clip_parse: invalid type %d of \"paths\" elements, must be string",
			paths->type);
		return VOD_BAD_MAPPING;
	}

	if (params[CONCAT_PARAM_DURATIONS] == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_clip_parse: \"durations\" is mandatory for concat");
		return VOD_BAD_MAPPING;
	}

	if (paths->count != params[CONCAT_PARAM_DURATIONS]->v.arr.count)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_clip_parse: \"paths\" element count %uz different than \"durations\" element count %uz", 
			paths->count,
			params[CONCAT_PARAM_DURATIONS]->v.arr.count);
		return VOD_BAD_MAPPING;
	}

	if (paths->count > MAX_CONCAT_ELEMENTS)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_clip_parse: number of concat elements %uz too big",
			paths->count);
		return VOD_BAD_MAPPING;
	}

	// initialize the tracks mask
	if (params[CONCAT_PARAM_TRACKS] != NULL)
	{
		src_str = &params[CONCAT_PARAM_TRACKS]->v.str;
		end_pos = src_str->data + src_str->len;
		vod_memzero(tracks_mask, sizeof(tracks_mask));
		if (parse_utils_extract_track_tokens(src_str->data, end_pos, tracks_mask) != end_pos)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_clip_parse: failed to parse tracks specification");
			return VOD_BAD_MAPPING;
		}
	}
	else
	{
		vod_memset(tracks_mask, 0xff, sizeof(tracks_mask));
	}

	if (context->range == NULL)
	{
		// no range, just use the last clip
		if (paths->count <= 0)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_clip_parse: \"paths\" array is empty");
			return VOD_BAD_MAPPING;
		}

		min_index = paths->count - 1;
		clip_count = 1;
		range = NULL;

		// allocate the source
		sources = vod_alloc(context->request_context->pool, sizeof(sources[0]));
		if (sources == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"concat_clip_parse: vod_alloc failed (1)");
			return VOD_ALLOC_FAILED;
		}
		sources_end = sources + 1;
		vod_memzero(sources, sizeof(sources[0]));
		sources->clip_to = context->duration;
	}
	else
	{
		durations = &params[CONCAT_PARAM_DURATIONS]->v.arr;
		if (durations->type != VOD_JSON_INT)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_clip_parse: invalid type %d of \"durations\" element, must be int",
				durations->type);
			return VOD_BAD_MAPPING;
		}

		start = context->range->start;
		end = context->range->end;

		// parse the offset
		if (params[CONCAT_PARAM_OFFSET] != NULL)
		{
			if (params[CONCAT_PARAM_OFFSET]->v.num.num < INT_MIN)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"concat_clip_parse: offset %L too small", params[CONCAT_PARAM_OFFSET]->v.num.num);
				return VOD_BAD_MAPPING;
			}

			offset = params[CONCAT_PARAM_OFFSET]->v.num.num;
		}
		else
		{
			offset = 0;
		}

		offset -= context->clip_from;

		min_index = UINT_MAX;
		max_index = durations->count - 1;
		part = &durations->part;
		for (i = 0, cur_duration = part->first;
			; 
			i++, cur_duration++, offset = next_offset)
		{
			if ((void*)cur_duration >= part->last)
			{
				if (part->next == NULL)
				{
					break;
				}

				part = part->next;
				cur_duration = part->first;
			}

			// validate the current duration element
			cur_duration_value = *cur_duration;
			if (cur_duration_value < 0)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"concat_clip_parse: negative duration value");
				return VOD_BAD_MAPPING;
			}

			if (cur_duration_value > INT_MAX - vod_max(offset, 0))
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"concat_clip_parse: duration value %uL too big",
					cur_duration_value);
				return VOD_BAD_MAPPING;
			}

			// update the min/max indexes
			next_offset = offset + cur_duration_value;
			if (next_offset <= (int64_t)start)
			{
				continue;
			}

			if (min_index == UINT_MAX)
			{
				min_index = i;
				start_offset = offset;
				first_part = part;
				first_duration = cur_duration;
			}

			if (next_offset >= (int64_t)end)
			{
				max_index = i;
				break;
			}
		}

		if (min_index == UINT_MAX)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_clip_parse: start offset %uL greater than the sum of the durations array",
				start);
			return VOD_BAD_MAPPING;
		}

		// allocate the sources and ranges
		clip_count = max_index - min_index + 1;
		sources = vod_alloc(context->request_context->pool,
			(sizeof(sources[0]) + sizeof(range[0])) * clip_count);
		if (sources == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"concat_clip_parse: vod_alloc failed (2)");
			return VOD_ALLOC_FAILED;
		}
		sources_end = sources + clip_count;
		vod_memzero(sources, sizeof(sources[0]) * clip_count);

		range = (void*)sources_end;

		// initialize the ranges
		original_clip_time = context->range->original_clip_time + start_offset;
		part = first_part;
		for (cur_source = sources, range_cur = range, cur_duration = first_duration;
			cur_source < sources_end;
			cur_source++, range_cur++, cur_duration++)
		{
			if ((void*)cur_duration >= part->last)
			{
				part = part->next;
				cur_duration = part->first;
			}

			range_cur->start = 0;
			range_cur->end = *cur_duration;
			range_cur->timescale = 1000;
			range_cur->original_clip_time = original_clip_time;
			original_clip_time += *cur_duration;

			cur_source->clip_to = *cur_duration;
		}

		if ((int64_t)start > start_offset)
		{
			range[0].start = start - start_offset;
		}

		if ((int64_t)end <= offset)
		{
			// if the start offset is greater than the range end, produce an empty clip
			// Note: there will always be a single clip in this case because if offset >= end,
			//		then next_offset >= end, and the for loop above will stop after the first iteration
			range[0].end = range[0].start;
		}
		else if (range[clip_count - 1].end > end - offset)
		{
			range[clip_count - 1].end = end - offset;
		}

		// parse the notifications
		if (params[CONCAT_PARAM_NOTIFICATIONS] != NULL)
		{
			rc = media_set_parse_notifications(
				context->request_context,
				&params[CONCAT_PARAM_NOTIFICATIONS]->v.arr,
				start,
				end,
				&context->notifications_head);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}

	// decode the base path
	base_path.len = 0;
	if (params[CONCAT_PARAM_BASE_PATH] != NULL)
	{
		base_path.data = vod_alloc(context->request_context->pool, params[CONCAT_PARAM_BASE_PATH]->v.str.len);
		if (base_path.data == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"concat_clip_parse: vod_alloc failed (3)");
			return VOD_ALLOC_FAILED;
		}

		rc = vod_json_decode_string(&base_path, &params[CONCAT_PARAM_BASE_PATH]->v.str);
		if (rc != VOD_JSON_OK)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_clip_parse: vod_json_decode_string failed %i", rc);
			return VOD_BAD_MAPPING;
		}
	}

	// find the first path element
	i = min_index;
	part = &paths->part;
	while (i >= part->count)
	{
		i -= part->count;
		part = part->next;
	}
	src_str = (vod_str_t*)part->first + i;

	cur_source = sources;
	clip_time = context->clip_time + start_offset;
	for (;;)
	{
		if ((void*)src_str >= part->last)
		{
			part = part->next;
			src_str = part->first;
		}

		// decode the path
		dest_str.data = vod_alloc(context->request_context->pool, base_path.len + src_str->len + 1);
		if (dest_str.data == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"concat_clip_parse: vod_alloc failed (4)");
			return VOD_ALLOC_FAILED;
		}

		vod_memcpy(dest_str.data, base_path.data, base_path.len);
		dest_str.len = base_path.len;

		rc = vod_json_decode_string(&dest_str, src_str);
		if (rc != VOD_JSON_OK)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_clip_parse: vod_json_decode_string failed %i", rc);
			return VOD_BAD_MAPPING;
		}

		dest_str.data[dest_str.len] = '\0';

		// initialize the source
		cur_source->next = sources_list_head;
		sources_list_head = cur_source;

		cur_source->base.type = MEDIA_CLIP_SOURCE;

		vod_memcpy(cur_source->tracks_mask, tracks_mask, sizeof(tracks_mask));
		cur_source->sequence = context->sequence;
		cur_source->range = range;
		cur_source->clip_time = clip_time;
		cur_source->stripped_uri = cur_source->mapped_uri = dest_str;

		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"concat_clip_parse: parsed clip source - path=%V tracks[v]=0x%uxL tracks[a]=0x%uxL",
			&cur_source->mapped_uri,
			cur_source->tracks_mask[MEDIA_TYPE_VIDEO][0],
			cur_source->tracks_mask[MEDIA_TYPE_AUDIO][0]);

		cur_source++;
		if (cur_source >= sources_end)
		{
			break;
		}

		clip_time += range->end;
		range++;
		src_str++;
	}

	// in case of a single clip, just return the first source (no need for a concat clip)
	if (clip_count == 1)
	{
		*result = &sources[0].base;
		goto done;
	}

	// return a concat clip
	clip = vod_alloc(context->request_context->pool, sizeof(*clip) + sizeof(media_clip_t*) * clip_count);
	if (clip == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"concat_clip_parse: vod_alloc failed (5)");
		return VOD_ALLOC_FAILED;
	}

	clip->base.type = MEDIA_CLIP_CONCAT;
	clip->base.audio_filter = NULL;

	clip->base.sources = (void*)(clip + 1);
	for (i = 0; i < clip_count; i++)
	{
		clip->base.sources[i] = &sources[i].base;
	}
	clip->base.source_count = clip_count;

	*result = &clip->base;

done:

	if (params[CONCAT_PARAM_PATHS] != NULL)
	{
		context->sources_head = sources_list_head;
	}
	else if (params[CONCAT_PARAM_CLIP_IDS] != NULL)
	{
		context->mapped_sources_head = sources_list_head;
	}

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"concat_clip_parse: done");
	return VOD_OK;
}

vod_status_t
concat_clip_concat(
	request_context_t* request_context, 
	media_clip_t* clip)
{
	media_clip_source_t* dest_clip;
	media_clip_source_t* src_clip;
	media_track_t* dest_track;
	media_track_t* src_track;
	media_clip_t** cur_source;
	uint32_t media_type;
	uint32_t i;

	for (cur_source = clip->sources + clip->source_count - 2; cur_source >= clip->sources; cur_source--)
	{
		dest_clip = (media_clip_source_t*)cur_source[0];
		src_clip = (media_clip_source_t*)cur_source[1];

		// verify the number of video/audio tracks match
		for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
		{
			if (src_clip->track_array.track_count[media_type] == dest_clip->track_array.track_count[media_type])
			{
				continue;
			}

			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"concat_clip_concat: concatenated sources have a different number of %uD tracks %uD vs %uD",
				media_type,
				src_clip->track_array.track_count[media_type],
				dest_clip->track_array.track_count[media_type]);
			return VOD_BAD_MAPPING;
		}

		// merge the frame parts
		for (i = 0; i < src_clip->track_array.total_track_count; i++)
		{
			src_track = &src_clip->track_array.first_track[i];
			if (src_track->frame_count <= 0)
			{
				continue;
			}

			dest_track = &dest_clip->track_array.first_track[i];
			if (dest_track->frame_count > 0)
			{
				dest_track->frames.next = &src_track->frames;
			}
			else
			{
				dest_track->frames = src_track->frames;
				dest_track->first_frame_index = src_track->first_frame_index;
				dest_track->first_frame_time_offset = src_track->first_frame_time_offset;
				dest_track->clip_start_time = src_track->clip_start_time;
				dest_track->clip_from_frame_offset = src_track->clip_from_frame_offset;
			}
			dest_track->frame_count += src_track->frame_count;
			dest_track->key_frame_count += src_track->key_frame_count;
			dest_track->total_frames_duration += src_track->total_frames_duration;
			dest_track->total_frames_size += src_track->total_frames_size;
		}
	}

	clip->source_count = 1;
	return VOD_OK;
}

vod_status_t
concat_clip_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"concat_clip_hash",
		concat_clip_params,
		sizeof(concat_clip_params[0]),
		&concat_clip_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
