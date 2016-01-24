#include "concat_filter.h"
#include "../media_set_parser.h"
#include "../parse_utils.h"

// constants
#define MAX_CONCAT_ELEMENTS (10000)

// enums
enum {
	CONCAT_PARAM_PATHS,
	CONCAT_PARAM_DURATIONS,
	CONCAT_PARAM_TRACKS,

	CONCAT_PARAM_COUNT
};

// typedefs
typedef struct {
	media_clip_t base;
} media_clip_concat_filter_t;

// constants
static json_object_key_def_t concat_filter_params[] = {
	{ vod_string("paths"),		VOD_JSON_ARRAY,		CONCAT_PARAM_PATHS },
	{ vod_string("durations"),	VOD_JSON_ARRAY,		CONCAT_PARAM_DURATIONS },
	{ vod_string("tracks"),		VOD_JSON_STRING,	CONCAT_PARAM_TRACKS },
	{ vod_null_string, 0, 0 }
};

// globals
static vod_hash_t concat_filter_hash;

vod_status_t
concat_filter_parse(
	void* ctx,
	vod_json_value_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	media_clip_concat_filter_t* filter;
	media_clip_source_t** source_ptr;
	media_clip_source_t* cur_source;
	media_clip_source_t* sources;
	vod_json_value_t* array_elts;
	vod_json_value_t* params[CONCAT_PARAM_COUNT];
	media_range_t* range;
	vod_array_t* array;
	vod_str_t* src_str;
	u_char* end_pos;
	vod_str_t dest_str;
	uint64_t start;
	uint64_t end;
	uint32_t tracks_mask[MEDIA_TYPE_COUNT];
	uint32_t min_index;
	uint32_t max_index;
	uint32_t clip_count;
	uint32_t cur_duration;
	uint32_t start_offset = 0;
	uint32_t next_offset;
	uint32_t offset;
	uint32_t i;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"concat_filter_parse: started");

	// get the required fields
	vod_json_get_object_values(
		element,
		&concat_filter_hash,
		params);

	// validate the paths and durations arrays
	if (params[CONCAT_PARAM_PATHS] == NULL || params[CONCAT_PARAM_DURATIONS] == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_filter_parse: \"paths\" and \"durations\" are mandatory for concat");
		return VOD_BAD_MAPPING;
	}

	if (params[CONCAT_PARAM_PATHS]->v.arr.nelts != params[CONCAT_PARAM_DURATIONS]->v.arr.nelts)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_filter_parse: \"paths\" element count %ui different than \"durations\" element count %ui", 
			params[CONCAT_PARAM_PATHS]->v.arr.nelts,
			params[CONCAT_PARAM_DURATIONS]->v.arr.nelts);
		return VOD_BAD_MAPPING;
	}

	if (params[CONCAT_PARAM_PATHS]->v.arr.nelts > MAX_CONCAT_ELEMENTS)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"concat_filter_parse: number of concat elements %ui too big",
			params[CONCAT_PARAM_PATHS]->v.arr.nelts);
		return VOD_BAD_MAPPING;
	}

	if (context->range == NULL)
	{
		// no range, just use the first clip
		if (params[CONCAT_PARAM_PATHS]->v.arr.nelts <= 0)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_filter_parse: \"paths\" array is empty");
			return VOD_BAD_MAPPING;
		}

		min_index = 0;
		max_index = 0;
		clip_count = 1;
		range = NULL;
	}
	else
	{
		array = &params[CONCAT_PARAM_DURATIONS]->v.arr;
		array_elts = array->elts;

		start = context->range->start;
		end = context->range->end;

		offset = 0;
		min_index = UINT_MAX;
		max_index = array->nelts - 1;
		for (i = 0; i < array->nelts; i++, offset = next_offset)
		{
			// validate the current duration element
			if (array_elts[i].type != VOD_JSON_INT)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"concat_filter_parse: invalid type %d of \"durations\" element, must be int", 
					array_elts[i].type);
				return VOD_BAD_MAPPING;
			}

			if (array_elts[i].v.num.nom > INT_MAX - offset)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"concat_filter_parse: duration value %uL too big",
					array_elts[i].v.num.nom);
				return VOD_BAD_MAPPING;
			}

			cur_duration = array_elts[i].v.num.nom;

			// update the min/max indexes
			next_offset = offset + cur_duration;
			if (next_offset <= start)
			{
				continue;
			}

			if (min_index == UINT_MAX)
			{
				min_index = i;
				start_offset = offset;
			}

			if (next_offset >= end)
			{
				max_index = i;
				break;
			}
		}

		if (min_index == UINT_MAX)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_filter_parse: start offset %uL greater than the sum of the durations array",
				start);
			return VOD_BAD_MAPPING;
		}

		// initialize the new range
		clip_count = max_index - min_index + 1;

		range = vod_alloc(context->request_context->pool, sizeof(range[0]) * clip_count);
		if (range == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"concat_filter_parse: vod_alloc failed (1)");
			return VOD_ALLOC_FAILED;
		}

		for (i = 0; i < clip_count; i++)
		{
			range[i].start = 0;
			range[i].end = array_elts[i + min_index].v.num.nom;
			range[i].timescale = 1000;
		}

		range[0].start = start - start_offset;
		range[clip_count - 1].end = end - offset;
	}

	// initialize the tracks mask
	if (params[CONCAT_PARAM_TRACKS] != NULL)
	{
		src_str = &params[CONCAT_PARAM_TRACKS]->v.str;
		end_pos = src_str->data + src_str->len;
		tracks_mask[MEDIA_TYPE_AUDIO] = 0;
		tracks_mask[MEDIA_TYPE_VIDEO] = 0;
		if (parse_utils_extract_track_tokens(src_str->data, end_pos, tracks_mask) != end_pos)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_filter_parse: failed to parse tracks specification");
			return VOD_BAD_MAPPING;
		}
	}
	else
	{
		tracks_mask[MEDIA_TYPE_AUDIO] = 0xffffffff;
		tracks_mask[MEDIA_TYPE_VIDEO] = 0xffffffff;
	}

	// allocate the sources and source pointers
	sources = vod_alloc(context->request_context->pool, sizeof(sources[0]) * clip_count);
	if (sources == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"concat_filter_parse: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	vod_memzero(sources, sizeof(sources[0]) * clip_count);
	cur_source = sources;

	source_ptr = vod_array_push_n(&context->sources, clip_count);
	if (source_ptr == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"concat_filter_parse: vod_array_push_n failed");
		return VOD_ALLOC_FAILED;
	}

	offset = context->sequence_offset + start_offset;

	array_elts = params[CONCAT_PARAM_PATHS]->v.arr.elts;
	for (i = min_index; i <= max_index; i++)
	{
		// decode the path
		if (array_elts[i].type != VOD_JSON_STRING)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_filter_parse: invalid type %d of \"paths\" element, must be string",
				array_elts[i].type);
			return VOD_BAD_MAPPING;
		}

		src_str = &array_elts[i].v.str;

		dest_str.data = vod_alloc(context->request_context->pool, src_str->len + 1);
		if (dest_str.data == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"concat_filter_parse: vod_alloc failed (3)");
			return VOD_ALLOC_FAILED;
		}

		rc = vod_json_decode_string(&dest_str, src_str);
		if (rc != VOD_JSON_OK)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"concat_filter_parse: vod_json_decode_string failed %i", rc);
			return VOD_BAD_MAPPING;
		}

		dest_str.data[dest_str.len] = '\0';

		// initialize the source
		*source_ptr++ = cur_source;

		cur_source->base.type = MEDIA_CLIP_SOURCE;

		cur_source->tracks_mask[MEDIA_TYPE_AUDIO] = tracks_mask[MEDIA_TYPE_AUDIO];
		cur_source->tracks_mask[MEDIA_TYPE_VIDEO] = tracks_mask[MEDIA_TYPE_VIDEO];
		cur_source->sequence = context->sequence;
		cur_source->range = range;
		cur_source->sequence_offset = offset;
		cur_source->stripped_uri = cur_source->mapped_uri = dest_str;

		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"concat_filter_parse: parsed clip source - path=%V tracks[v]=0x%uxD tracks[a]=0x%uxD",
			&cur_source->mapped_uri,
			cur_source->tracks_mask[MEDIA_TYPE_VIDEO],
			cur_source->tracks_mask[MEDIA_TYPE_AUDIO]);

		if (range == NULL)
		{
			cur_source->clip_to = context->duration;
			break;
		}

		cur_source->clip_to = range->end;
		cur_source++;
		offset += range->end;
		range++;
	}

	// in case of a single clip, just return the first source (no need for a concat filter)
	if (clip_count == 1)
	{
		*result = &sources[0].base;
		goto done;
	}

	// return a concat filter
	filter = vod_alloc(context->request_context->pool, sizeof(*filter) + sizeof(media_clip_t*) * clip_count);
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"concat_filter_parse: vod_alloc failed (4)");
		return VOD_ALLOC_FAILED;
	}

	filter->base.type = MEDIA_CLIP_CONCAT_FILTER;
	filter->base.audio_filter = NULL;

	filter->base.sources = (void*)(filter + 1);
	for (i = 0; i < clip_count; i++)
	{
		filter->base.sources[i] = &sources[i].base;
	}
	filter->base.source_count = clip_count;

	*result = &filter->base;

done:

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"concat_filter_parse: done");
	return VOD_OK;
}

vod_status_t
concat_filter_concat(
	request_context_t* request_context, 
	media_clip_t* clip)
{
	media_clip_source_t* dest_clip;
	media_clip_source_t* src_clip;
	media_clip_t** cur_source;
	uint32_t i;

	for (cur_source = clip->sources + clip->source_count - 2; cur_source >= clip->sources; cur_source--)
	{
		dest_clip = (media_clip_source_t*)cur_source[0];
		src_clip = (media_clip_source_t*)cur_source[1];

		// verify the number of video/audio tracks match
		if (src_clip->track_array.track_count[MEDIA_TYPE_VIDEO] != dest_clip->track_array.track_count[MEDIA_TYPE_VIDEO])
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"concat_filter_concat: concatenated sources have a different number of video tracks %uD vs %uD",
				src_clip->track_array.track_count[MEDIA_TYPE_VIDEO],
				dest_clip->track_array.track_count[MEDIA_TYPE_VIDEO]);
			return VOD_BAD_MAPPING;
		}

		if (src_clip->track_array.track_count[MEDIA_TYPE_AUDIO] != dest_clip->track_array.track_count[MEDIA_TYPE_AUDIO])
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"concat_filter_concat: concatenated sources have a different number of audio tracks %uD vs %uD",
				src_clip->track_array.track_count[MEDIA_TYPE_AUDIO],
				dest_clip->track_array.track_count[MEDIA_TYPE_AUDIO]);
			return VOD_BAD_MAPPING;
		}

		// merge the frame parts
		for (i = 0; i < src_clip->track_array.total_track_count; i++)
		{
			dest_clip->track_array.first_track[i].frames.next = &src_clip->track_array.first_track[i].frames;
		}
	}

	clip->source_count = 1;
	return VOD_OK;
}

vod_status_t
concat_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"concat_filter_hash",
		concat_filter_params,
		sizeof(concat_filter_params[0]),
		&concat_filter_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
