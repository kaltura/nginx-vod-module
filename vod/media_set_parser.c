#include "media_set_parser.h"
#include "json_parser.h"
#include "segmenter.h"
#include "filters/gain_filter.h"
#include "filters/rate_filter.h"
#include "filters/mix_filter.h"
#include "parse_utils.h"

// typedefs
enum {
	MEDIA_SET_PARAM_DISCONTINUITY,
	MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO,
	MEDIA_SET_PARAM_DURATIONS,
	MEDIA_SET_PARAM_SEQUENCES,

	MEDIA_SET_PARAM_COUNT
};

typedef struct {
	media_filter_parse_context_t base;
	get_clip_ranges_result_t clip_ranges;
	media_set_t* media_set;
	vod_array_t sources;
	uint32_t clip_id;
} media_set_parse_context_t;

// forward decls
static vod_status_t media_set_parse_null_term_string(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_tracks_spec(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_int32(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_encryption_key(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_source(void* ctx, vod_json_value_t* element, void** result);
static vod_status_t media_set_parse_sequence_clips(void* ctx, vod_json_value_t* value, void* dest);

// constants
static json_object_value_def_t media_clip_source_params[] = {
	{ vod_string("path"), VOD_JSON_STRING, offsetof(media_clip_source_t, mapped_uri), media_set_parse_null_term_string },
	{ vod_string("tracks"), VOD_JSON_STRING, offsetof(media_clip_source_t, tracks_mask), media_set_parse_tracks_spec },
	{ vod_string("clipFrom"), VOD_JSON_INT, offsetof(media_clip_source_t, clip_from), media_set_parse_int32 },
	{ vod_string("encryptionKey"), VOD_JSON_STRING, offsetof(media_clip_source_t, encryption_key), media_set_parse_encryption_key },
	{ vod_null_string, 0, 0, NULL }
};

static json_parser_union_type_def_t media_clip_union_types[] = {
	{ vod_string("gainFilter"), gain_filter_parse },
	{ vod_string("mixFilter"), mix_filter_parse },
	{ vod_string("rateFilter"), rate_filter_parse },
	{ vod_string("source"), media_set_parse_source },
	{ vod_null_string, NULL }
};

static json_object_value_def_t media_sequence_params[] = {
	{ vod_string("clips"), VOD_JSON_ARRAY, offsetof(media_sequence_t, clips), media_set_parse_sequence_clips },
	{ vod_null_string, 0, 0, NULL }
};

static json_object_key_def_t media_set_params[] = {
	{ vod_string("discontinuity"),					VOD_JSON_BOOL,	MEDIA_SET_PARAM_DISCONTINUITY },
	{ vod_string("consistentSequenceMediaInfo"),	VOD_JSON_BOOL,	MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO },
	{ vod_string("durations"),						VOD_JSON_ARRAY, MEDIA_SET_PARAM_DURATIONS },
	{ vod_string("sequences"),						VOD_JSON_ARRAY,	MEDIA_SET_PARAM_SEQUENCES },
	{ vod_null_string, 0, 0 }
};

static vod_str_t type_key = vod_string("type");
static vod_uint_t type_key_hash = vod_hash(vod_hash(vod_hash('t', 'y'), 'p'), 'e');

// globals
static vod_hash_t media_clip_source_hash;
static vod_hash_t media_clip_union_hash;
static vod_hash_t media_sequence_hash;
static vod_hash_t media_set_hash;

static vod_status_t
media_set_parse_durations(
	request_context_t* request_context,
	vod_array_t* array,
	media_set_t* result)
{
	vod_json_value_t* cur_pos;
	vod_json_value_t* end_pos;
	uint32_t* cur_output;
	uint64_t total_duration = 0;

	if (array->nelts < 1 || array->nelts > MAX_CLIPS)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_durations: invalid number of elements in the durations array %ui", array->nelts);
		return VOD_BAD_MAPPING;
	}

	cur_output = vod_alloc(request_context->pool, sizeof(result->durations[0]) * array->nelts);
	if (cur_output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"media_set_parse_durations: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->durations = cur_output;

	cur_pos = array->elts;
	end_pos = cur_pos + array->nelts;
	for (; cur_pos < end_pos; cur_pos++, cur_output++)
	{
		if (cur_pos->type != VOD_JSON_INT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_durations: invalid duration type %d expected int", cur_pos->type);
			return VOD_BAD_MAPPING;
		}

		if (cur_pos->v.num.nom <= 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_durations: invalid duration %L must be positive", cur_pos->v.num.nom);
			return VOD_BAD_MAPPING;
		}

		*cur_output = cur_pos->v.num.nom;
		total_duration += cur_pos->v.num.nom;
	}

	result->total_clip_count = array->nelts;
	result->total_duration = total_duration;

	return VOD_OK;
}

static vod_status_t
media_set_parse_int32(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	*(uint32_t*)dest = value->v.num.nom;

	return VOD_OK;
}

static vod_status_t
media_set_parse_encryption_key(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_filter_parse_context_t* context = ctx;
	u_char* result;

	result = vod_alloc(context->request_context->pool, MP4_AES_CTR_KEY_SIZE);
	if (result == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"media_set_parse_encryption_key: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	*(u_char**)dest = result;

	return parse_utils_parse_fixed_base64_string(&value->v.str, result, MP4_AES_CTR_KEY_SIZE);
}

static vod_status_t 
media_set_parse_null_term_string(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_filter_parse_context_t* context = ctx;
	vod_str_t result;

	result.len = value->v.str.len;
	result.data = vod_alloc(context->request_context->pool, result.len + 1);
	if (result.data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"media_set_parse_null_term_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(result.data, value->v.str.data, result.len);
	result.data[result.len] = '\0';

	*(vod_str_t*)dest = result;

	return VOD_OK;
}

static vod_status_t
media_set_parse_tracks_spec(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_filter_parse_context_t* context = ctx;
	uint32_t* tracks_mask = dest;
	u_char* end_pos = value->v.str.data + value->v.str.len;

	tracks_mask[MEDIA_TYPE_AUDIO] = 0;
	tracks_mask[MEDIA_TYPE_VIDEO] = 0;
	if (parse_utils_extract_track_tokens(value->v.str.data, end_pos, tracks_mask) != end_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_tracks_spec: failed to parse tracks specification");
		return VOD_BAD_MAPPING;
	}

	return VOD_OK;
}

static vod_status_t
media_set_parse_source(
	void* ctx,
	vod_json_value_t* element,
	void** result)
{
	media_set_parse_context_t* context = ctx;
	media_clip_source_t** source_ptr;
	media_clip_source_t* source;
	vod_status_t rc;

	source = vod_alloc(context->base.request_context->pool, sizeof(*source));
	if (source == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_source: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	source_ptr = vod_array_push(&context->sources);
	if (source_ptr == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_source: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}
	*source_ptr = source;

	vod_memzero(source, sizeof(*source));

	source->base.type = MEDIA_CLIP_SOURCE;

	source->tracks_mask[MEDIA_TYPE_AUDIO] = 0xffffffff;
	source->tracks_mask[MEDIA_TYPE_VIDEO] = 0xffffffff;
	source->sequence = context->base.sequence;
	source->range = context->base.range;
	source->sequence_offset = context->base.sequence_offset;
	source->mapped_uri.len = (size_t)-1;

	rc = vod_json_parse_object_values(element, &media_clip_source_hash, context, source);
	if (rc != VOD_OK)
	{
		return rc;
	}

	switch (source->mapped_uri.len)
	{
	case (size_t)-1:
		vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
			"media_set_parse_source: missing path in source object");
		return VOD_BAD_MAPPING;

	case 0:
		vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
			"media_set_parse_source: empty path in source object %V", &context->media_set->uri);
		return VOD_NOT_FOUND;
	}

	source->clip_to = source->clip_from + context->base.duration;

	vod_log_debug4(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
		"media_set_parse_source: parsed clip source - path=%V tracks[v]=0x%uxD tracks[a]=0x%uxD, clipFrom=%uD", 
		&source->mapped_uri, 
		source->tracks_mask[MEDIA_TYPE_VIDEO],
		source->tracks_mask[MEDIA_TYPE_AUDIO],
		source->clip_from);

	source->stripped_uri = source->mapped_uri;

	*result = &source->base;

	return VOD_OK;
}

vod_status_t
media_set_parse_clip(
	void* ctx,
	vod_json_value_t* element,
	media_clip_t* parent,
	media_clip_t** result)
{
	media_set_parse_context_t* context = ctx;
	vod_status_t rc;

	rc = vod_json_parse_union(
		context->base.request_context,
		element,
		&type_key,
		type_key_hash,
		&media_clip_union_hash,
		context,
		(void**)result);
	if (rc != VOD_OK)
	{
		return rc;
	}

	(*result)->parent = parent;
	(*result)->id = context->clip_id++;

	return VOD_OK;
}

vod_status_t 
media_set_parse_filter_sources(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_filter_parse_context_t* context = ctx;
	vod_json_value_t* sources_cur;
	vod_json_value_t* sources_end;
	vod_array_t* sources = &value->v.arr;
	media_clip_t** output;
	media_clip_t* filter = dest;
	vod_status_t rc;

	if (sources->nelts < 1 || sources->nelts > MAX_SOURCES)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_filter_sources: invalid number of elements in the sources array %ui", sources->nelts);
		return VOD_BAD_MAPPING;
	}

	filter->source_count = sources->nelts;
	filter->sources = vod_alloc(context->request_context->pool, sizeof(filter->sources[0]) * filter->source_count);
	if (filter->sources == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"media_set_parse_filter_sources: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	sources_cur = sources->elts;
	sources_end = sources_cur + sources->nelts;
	output = filter->sources;
	for (; sources_cur < sources_end; sources_cur++, output++)
	{
		if (sources_cur->type != VOD_JSON_OBJECT)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"media_set_parse_filter_sources: invalid source type %d expected object", sources_cur->type);
			return VOD_BAD_MAPPING;
		}

		rc = media_set_parse_clip(
			ctx,
			sources_cur,
			filter,
			output);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t 
media_set_parse_sequence_clips(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_set_parse_context_t* context = ctx;
	media_clip_t*** clips = dest;
	vod_json_value_t* cur_pos;
	vod_json_value_t* end_pos;
	vod_array_t* array = &value->v.arr;
	media_clip_t** cur_output;
	vod_status_t rc;
	uint32_t* cur_duration;

	if (array->nelts != context->media_set->total_clip_count)
	{
		vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
			"media_set_parse_sequence_clips: sequence clips count %ui does not match the durations count %uD",
			array->nelts, context->media_set->total_clip_count);
		return VOD_BAD_MAPPING;
	}

	cur_output = vod_alloc(context->base.request_context->pool, sizeof(cur_output[0]) * context->clip_ranges.clip_count);
	if (cur_output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_sequence_clips: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	*clips = cur_output;

	context->base.sequence_offset = context->clip_ranges.initial_sequence_offset;
	context->base.range = context->clip_ranges.clip_ranges;
	cur_duration = context->media_set->durations + context->clip_ranges.min_clip_index;
	cur_pos = (vod_json_value_t*)array->elts + context->clip_ranges.min_clip_index;
	end_pos = (vod_json_value_t*)array->elts + context->clip_ranges.max_clip_index;
	for (; cur_pos <= end_pos; cur_pos++)
	{
		if (cur_pos->type != VOD_JSON_OBJECT)
		{
			vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
				"media_set_parse_sequence_clips: invalid clip type %d expected object", cur_pos->type);
			return VOD_BAD_MAPPING;
		}

		context->base.duration = cur_duration != NULL ? *cur_duration : UINT_MAX;

		rc = media_set_parse_clip(
			context, 
			cur_pos, 
			NULL, 
			cur_output);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// Note: in case the durations are null, sequence_offset and cur_duration become corrupted,
		//		but since there is always a single clip in this case, they won't be used
		context->base.sequence_offset += context->base.duration;
		if (context->base.range != NULL)
		{
			context->base.range++;
		}
		cur_output++;
		cur_duration++;
	}

	return VOD_OK;
}

static vod_status_t
media_set_parse_sequences(
	media_set_parse_context_t* context,
	vod_array_t* array, 
	request_params_t* request_params)
{
	vod_json_value_t* cur_pos;
	vod_json_value_t* end_pos;
	media_sequence_t* cur_output;
	vod_status_t rc;
	uint32_t required_sequences_num;
	uint32_t index;

	if (array->nelts < 1 || array->nelts > MAX_SEQUENCES)
	{
		vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
			"media_set_parse_sequences: invalid number of sequences %ui", array->nelts);
		return VOD_BAD_MAPPING;
	}

	required_sequences_num = vod_get_number_of_set_bits(request_params->sequences_mask);
	required_sequences_num = vod_min(array->nelts, required_sequences_num);

	cur_output = vod_alloc(
		context->base.request_context->pool, 
		sizeof(context->media_set->sequences[0]) * required_sequences_num);
	if (cur_output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_sequences: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	rc = vod_array_init(
		&context->sources, 
		context->base.request_context->pool, 
		required_sequences_num * context->media_set->clip_count,		// usually there is a single source per clip
		sizeof(context->media_set->sources[0]));
	if (rc != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_sequences: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	context->media_set->has_multi_sequences = array->nelts > 1;
	context->media_set->sequences = cur_output;

	index = 0;
	cur_pos = array->elts;
	end_pos = cur_pos + array->nelts;
	for (; cur_pos < end_pos; cur_pos++, index++)
	{
		if ((request_params->sequences_mask & (1 << index)) == 0)
		{
			continue;
		}

		if (cur_pos->type != VOD_JSON_OBJECT)
		{
			vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
				"media_set_parse_sequences: invalid sequence type %d expected object", cur_pos->type);
			return VOD_BAD_MAPPING;
		}

		context->base.sequence = cur_output;

		cur_output->clips = NULL;

		rc = vod_json_parse_object_values(
			cur_pos,
			&media_sequence_hash,
			context,
			cur_output);
		if (rc != VOD_OK)
		{
			return rc;
		}
		
		if (cur_output->clips == NULL)
		{
			vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
				"media_set_parse_sequences: missing clips in sequence object");
			return VOD_BAD_MAPPING;
		}

		cur_output->index = index;
		cur_output->mapped_uri.len = 0;
		cur_output->stripped_uri.len = 0;
		cur_output++;
	}

	context->media_set->sequences_end = cur_output;
	context->media_set->sequence_count = cur_output - context->media_set->sequences;

	context->media_set->sources = context->sources.elts;
	context->media_set->sources_end = context->media_set->sources + context->sources.nelts;

	return VOD_OK;
}

vod_status_t
media_set_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"media_set_hash",
		media_set_params,
		sizeof(media_set_params[0]),
		&media_set_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"media_sequence_hash",
		media_sequence_params,
		sizeof(media_sequence_params[0]),
		&media_sequence_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"media_clip_source_hash",
		media_clip_source_params,
		sizeof(media_clip_source_params[0]),
		&media_clip_source_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"media_clip_union_hash",
		media_clip_union_types,
		sizeof(media_clip_union_types[0]),
		&media_clip_union_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = gain_filter_parser_init(pool, temp_pool);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = mix_filter_parser_init(pool, temp_pool);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = rate_filter_parser_init(pool, temp_pool);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

vod_status_t
media_set_parse_json(
	request_context_t* request_context, 
	u_char* string, 
	request_params_t* request_params,
	segmenter_conf_t* segmenter,
	vod_str_t* uri,
	bool_t parse_all_clips,
	media_set_t* result)
{
	media_set_parse_context_t context;
	vod_json_value_t* params[MEDIA_SET_PARAM_COUNT];
	vod_json_value_t json;
	vod_status_t rc;
	uint32_t* cur_duration;
	uint32_t* duration_end;
	u_char error[128];
	
	result->uri = *uri;

	// parse the json and get the media set object values
	rc = vod_json_parse(request_context->pool, string, &json, error, sizeof(error));
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_json: failed to parse json %i: %s", rc, error);
		return VOD_BAD_MAPPING;
	}

	vod_memzero(params, sizeof(params));

	vod_json_get_object_values(
		&json,
		&media_set_hash,
		params);

	if (params[MEDIA_SET_PARAM_SEQUENCES] == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_json: \"sequences\" element is missing");
		return VOD_BAD_MAPPING;
	}

	if (params[MEDIA_SET_PARAM_DURATIONS] != NULL)
	{
		// durations
		rc = media_set_parse_durations(
			request_context,
			&params[MEDIA_SET_PARAM_DURATIONS]->v.arr,
			result);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// discontinuity
		if (params[MEDIA_SET_PARAM_DISCONTINUITY] != NULL)
		{
			result->use_discontinuity = params[MEDIA_SET_PARAM_DISCONTINUITY]->v.boolean;
		}
		else
		{
			result->use_discontinuity = result->total_clip_count > 1;
		}

		if (!result->use_discontinuity && 
			request_params->clip_index != INVALID_CLIP_INDEX &&
			request_params->clip_index != 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_json: clip index %uD not allowed in continuous mode", request_params->clip_index);
			return VOD_BAD_REQUEST;
		}

		if (request_params->segment_index != INVALID_SEGMENT_INDEX)
		{
			// clip start/end ranges
			if (result->use_discontinuity)
			{
				if (request_params->segment_time != INVALID_SEGMENT_TIME)
				{
					// recalculate the segment index if it was determined according to timestamp
					rc = segmenter_get_segment_index_discontinuity(
						request_context,
						segmenter,
						result->durations,
						result->total_clip_count,
						request_params->segment_time,
						&request_params->segment_index);
					if (rc != VOD_OK)
					{
						return rc;
					}
				}

				rc = segmenter_get_start_end_ranges_discontinuity(
					request_context,
					segmenter,
					request_params->clip_index,
					request_params->segment_index,
					result->durations,
					result->total_clip_count,
					result->total_duration,
					&context.clip_ranges);
			}
			else
			{				
				rc = segmenter_get_start_end_ranges_no_discontinuity(
					request_context,
					segmenter,
					request_params->segment_index,
					result->durations,
					result->total_clip_count,
					result->total_duration,
					result->total_duration,
					&context.clip_ranges);
			}

			if (rc != VOD_OK)
			{
				return rc;
			}

			// in case a clip index was passed on the request, adjust the segment index 
			//	to count from the beginning of the sequence
			request_params->segment_index += context.clip_ranges.clip_first_segment_index;
		}
		else
		{
			// not a segment request
			context.clip_ranges.clip_ranges = NULL;
			if (request_params->clip_index != INVALID_CLIP_INDEX)
			{
				// clip index specified on the request
				if (request_params->clip_index >= result->total_clip_count)
				{
					vod_log_error(VOD_LOG_ERR, request_context->log, 0,
						"media_set_parse_json: invalid clip index %uD greater than clip count %uD", 
						request_params->clip_index, result->total_clip_count);
					return VOD_BAD_REQUEST;
				}

				context.clip_ranges.clip_count = 1;
				context.clip_ranges.min_clip_index = request_params->clip_index;
				context.clip_ranges.max_clip_index = request_params->clip_index;

				duration_end = result->durations + request_params->clip_index;
				context.clip_ranges.initial_sequence_offset = 0;
				for (cur_duration = result->durations; cur_duration < duration_end; cur_duration++)
				{
					context.clip_ranges.initial_sequence_offset += *cur_duration;
				}
			}
			else
			{
				// clip index not specified on the request
				if (parse_all_clips ||
					(params[MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO] != NULL &&
					!params[MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO]->v.boolean))
				{
					// parse all clips
					context.clip_ranges.clip_count = result->total_clip_count;
					context.clip_ranges.max_clip_index = result->total_clip_count - 1;
				}
				else
				{
					// parse only the first clip in each sequence, assume subsequent clips have the same media info
					context.clip_ranges.clip_count = 1;
					context.clip_ranges.max_clip_index = 0;
				}

				context.clip_ranges.min_clip_index = 0;
				context.clip_ranges.initial_sequence_offset = 0;
			}
		}
	}
	else
	{
		// no durations in the json
		if (request_params->clip_index != INVALID_CLIP_INDEX &&
			request_params->clip_index != 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_json: invalid clip index %uD with single clip", request_params->clip_index);
			return VOD_BAD_REQUEST;
		}

		result->total_clip_count = 1;
		result->durations = NULL;
		result->use_discontinuity = FALSE;

		context.clip_ranges.clip_ranges = NULL;
		context.clip_ranges.clip_count = 1;
		context.clip_ranges.min_clip_index = 0;
		context.clip_ranges.max_clip_index = 0;
		context.clip_ranges.initial_sequence_offset = 0;
	}

	if (context.clip_ranges.clip_count > MAX_CLIPS_PER_REQUEST)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_json: clip count %uD exceeds the limit per request", context.clip_ranges.clip_count);
		return VOD_BAD_REQUEST;
	}

	result->clip_count = context.clip_ranges.clip_count;

	// sequences
	context.media_set = result;
	context.base.request_context = request_context;
	context.clip_id = 1;

	rc = media_set_parse_sequences(&context, &params[MEDIA_SET_PARAM_SEQUENCES]->v.arr, request_params);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
