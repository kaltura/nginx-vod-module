#include "media_set_parser.h"
#include "json_parser.h"
#include "segmenter.h"
#include "filters/gain_filter.h"
#include "filters/rate_filter.h"
#include "filters/mix_filter.h"
#include "parse_utils.h"

#define MAX_CLIP_DURATION (86400000)		// 1 day
#define MAX_SEQUENCE_DURATION (864000000)		// 10 days

// typedefs
enum {
	MEDIA_SET_PARAM_DISCONTINUITY,
	MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO,
	MEDIA_SET_PARAM_DURATIONS,
	MEDIA_SET_PARAM_SEQUENCES,
	MEDIA_SET_PARAM_INITIAL_SEGMENT_INDEX,
	MEDIA_SET_PARAM_INITIAL_CLIP_INDEX,
	MEDIA_SET_PARAM_FIRST_CLIP_TIME,
	MEDIA_SET_PARAM_SEGMENT_BASE_TIME,
	MEDIA_SET_PARAM_PLAYLIST_TYPE,

	MEDIA_SET_PARAM_COUNT
};

typedef struct {
	media_filter_parse_context_t base;
	get_clip_ranges_result_t clip_ranges;
	media_set_t* media_set;
	vod_array_t sources;
	uint32_t clip_id;
	uint32_t expected_clip_count;
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
	{ vod_string("initialSegmentIndex"),			VOD_JSON_INT,	MEDIA_SET_PARAM_INITIAL_SEGMENT_INDEX },
	{ vod_string("initialClipIndex"),				VOD_JSON_INT,	MEDIA_SET_PARAM_INITIAL_CLIP_INDEX },
	{ vod_string("firstClipTime"),					VOD_JSON_INT,	MEDIA_SET_PARAM_FIRST_CLIP_TIME },
	{ vod_string("segmentBaseTime"),				VOD_JSON_INT,	MEDIA_SET_PARAM_SEGMENT_BASE_TIME },
	{ vod_string("playlistType"),					VOD_JSON_STRING,MEDIA_SET_PARAM_PLAYLIST_TYPE },
	{ vod_null_string, 0, 0 }
};

static vod_str_t type_key = vod_string("type");
static vod_uint_t type_key_hash = vod_hash(vod_hash(vod_hash('t', 'y'), 'p'), 'e');

static vod_str_t playlist_type_vod = vod_string("vod");
static vod_str_t playlist_type_live = vod_string("live");

// globals
static vod_hash_t media_clip_source_hash;
static vod_hash_t media_clip_union_hash;
static vod_hash_t media_sequence_hash;
static vod_hash_t media_set_hash;

static vod_status_t
media_set_parse_durations(
	request_context_t* request_context,
	vod_array_t* array,
	media_set_t* media_set)
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

	cur_output = vod_alloc(request_context->pool, sizeof(media_set->durations[0]) * array->nelts);
	if (cur_output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"media_set_parse_durations: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	media_set->durations = cur_output;

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

		if (cur_pos->v.num.nom > MAX_CLIP_DURATION)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_durations: clip duration %L too large", cur_pos->v.num.nom);
			return VOD_BAD_MAPPING;
		}

		*cur_output = cur_pos->v.num.nom;
		total_duration += cur_pos->v.num.nom;
	}

	if (total_duration > MAX_SEQUENCE_DURATION)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_durations: total duration %uL too large", total_duration);
		return VOD_BAD_MAPPING;
	}

	media_set->total_clip_count = array->nelts;
	media_set->total_duration = total_duration;

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
#if (VOD_HAVE_OPENSSL_EVP)
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
#else
	vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
		"media_set_parse_encryption_key: decryption not supported, recompile with openssl to enable it");
	return VOD_BAD_REQUEST;
#endif //(VOD_HAVE_OPENSSL_EVP)
}

static vod_status_t 
media_set_parse_null_term_string(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_filter_parse_context_t* context = ctx;
	vod_json_status_t rc;
	vod_str_t result;

	result.data = vod_alloc(context->request_context->pool, value->v.str.len + 1);
	if (result.data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"media_set_parse_null_term_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	rc = vod_json_decode_string(&result, &value->v.str);
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_null_term_string: vod_json_decode_string failed %i", rc);
		return VOD_BAD_MAPPING;
	}

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

	if (array->nelts != context->expected_clip_count)
	{
		vod_log_error(VOD_LOG_ERR, context->base.request_context->log, 0,
			"media_set_parse_sequence_clips: sequence clips count %ui does not match the durations count %uD",
			array->nelts, context->expected_clip_count);
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

static vod_status_t
media_set_parse_live_params(
	request_context_t* request_context,
	request_params_t* request_params,
	segmenter_conf_t* segmenter,
	vod_json_value_t** params,
	media_set_t* media_set, 
	uint64_t* segment_base_time)
{
	// first clip time
	if (params[MEDIA_SET_PARAM_FIRST_CLIP_TIME] == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_live_params: firstClipTime missing in live playlist");
		return VOD_BAD_MAPPING;
	}

	media_set->first_clip_time = params[MEDIA_SET_PARAM_FIRST_CLIP_TIME]->v.num.nom;

	if (media_set->use_discontinuity)
	{
		// non-continuous - the upstream server has to keep state in order to have sequential segment/clip indexes
		//	(it is not possible to calculate the segment index without knowing all past clip durations)
		if (params[MEDIA_SET_PARAM_INITIAL_SEGMENT_INDEX] == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_live_params: initialSegmentIndex missing in non-continuous live playlist");
			return VOD_BAD_MAPPING;
		}

		if (params[MEDIA_SET_PARAM_INITIAL_CLIP_INDEX] == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_live_params: initialClipIndex missing in non-continuous live playlist");
			return VOD_BAD_MAPPING;
		}

		media_set->initial_segment_index = params[MEDIA_SET_PARAM_INITIAL_SEGMENT_INDEX]->v.num.nom - 1;
		media_set->initial_clip_segment_index = media_set->initial_segment_index;

		media_set->initial_clip_index = params[MEDIA_SET_PARAM_INITIAL_CLIP_INDEX]->v.num.nom - 1;
		if (request_params->clip_index != INVALID_CLIP_INDEX)
		{
			if (request_params->clip_index < media_set->initial_clip_index)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"media_set_parse_live_params: clip index %uD is smaller than the initial clip index %uD",
					request_params->clip_index, media_set->initial_clip_index);
				return VOD_BAD_REQUEST;
			}
			request_params->clip_index -= media_set->initial_clip_index;

			// adjust the segment index to the first clip, 
			// it will be further adjusted after the start/end ranges get calculated
			if (request_params->segment_index != INVALID_SEGMENT_INDEX)
			{
				request_params->segment_index += media_set->initial_segment_index;
			}
		}

		*segment_base_time = media_set->first_clip_time;
	}
	else
	{
		// continuous - segmentation is performed relative to some reference time (has to remain fixed per stream)
		if (params[MEDIA_SET_PARAM_SEGMENT_BASE_TIME] == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_live_params: segmentBaseTime missing in continuous live playlist");
			return VOD_BAD_MAPPING;
		}

		*segment_base_time = params[MEDIA_SET_PARAM_SEGMENT_BASE_TIME]->v.num.nom;

		if (*segment_base_time > media_set->first_clip_time)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_live_params: segment base time %uL is larger than first clip time %uL",
				*segment_base_time, media_set->first_clip_time);
			return VOD_BAD_MAPPING;
		}
	}

	return VOD_OK;
}

static vod_status_t
media_set_get_live_window(
	request_context_t* request_context,
	segmenter_conf_t* segmenter,
	media_set_t* media_set,
	uint64_t segment_base_time,
	bool_t parse_all_clips,
	get_clip_ranges_result_t* clip_ranges)
{
	get_clip_ranges_result_t max_clip_ranges;
	get_clip_ranges_result_t min_clip_ranges;
	vod_status_t rc;
	uint64_t current_time;
	uint64_t start_time;
	uint64_t end_time;
	uint32_t* durations_end;
	uint32_t* durations_cur;
	uint32_t min_segment_index;
	uint32_t max_segment_index;

	// non-segment request
	current_time = vod_time() * 1000;
	if (media_set->first_clip_time > current_time)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_get_live_window: first clip time %uL is larger than current time %uL",
			media_set->first_clip_time, current_time);
		return VOD_BAD_MAPPING;
	}

	// get the max segment index
	if (media_set->use_discontinuity)
	{
		rc = segmenter_get_segment_index_discontinuity(
			request_context,
			segmenter,
			media_set->initial_segment_index,
			media_set->durations,
			media_set->total_clip_count,
			current_time - segment_base_time,
			&max_segment_index);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	else
	{
		max_segment_index = segmenter_get_segment_index_no_discontinuity(
			segmenter,
			current_time - segment_base_time);
	}

	// get the min segment index
	if (max_segment_index > media_set->initial_segment_index + segmenter->live_segment_count - 1)
	{
		min_segment_index = max_segment_index - segmenter->live_segment_count + 1;
	}
	else
	{
		min_segment_index = media_set->initial_segment_index;
	}

	// get the ranges of the first and last segments
	if (media_set->use_discontinuity)
	{
		// TODO: consider optimizing this by creating dedicated segmenter functions
		rc = segmenter_get_start_end_ranges_discontinuity(
			request_context,
			segmenter,
			0,
			min_segment_index,
			media_set->initial_segment_index,
			media_set->durations,
			media_set->total_clip_count,
			media_set->total_duration,
			&min_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}

		rc = segmenter_get_start_end_ranges_discontinuity(
			request_context,
			segmenter,
			0,
			max_segment_index,
			media_set->initial_segment_index,
			media_set->durations,
			media_set->total_clip_count,
			media_set->total_duration,
			&max_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	else
	{
		start_time = media_set->first_clip_time - segment_base_time;
		end_time = start_time + media_set->total_duration;

		rc = segmenter_get_start_end_ranges_no_discontinuity(
			request_context,
			segmenter,
			min_segment_index,
			media_set->durations,
			media_set->total_clip_count,
			start_time,
			end_time,
			end_time,
			&min_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}

		rc = segmenter_get_start_end_ranges_no_discontinuity(
			request_context,
			segmenter,
			max_segment_index,
			media_set->durations,
			media_set->total_clip_count,
			start_time,
			end_time,
			end_time,
			&max_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	if (min_clip_ranges.clip_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_get_live_window: no clips found for the min segment %uD", min_segment_index);
		return VOD_BAD_MAPPING;
	}

	if (max_clip_ranges.clip_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_get_live_window: no clips found for the max segment %uD", max_segment_index);
		return VOD_BAD_MAPPING;
	}

	// trim the durations array
	// Note: min_clip_index and max_clip_index can be identical
	media_set->durations[max_clip_ranges.max_clip_index] = max_clip_ranges.clip_ranges[max_clip_ranges.clip_count - 1].end;
	media_set->durations[min_clip_ranges.min_clip_index] -= min_clip_ranges.clip_ranges[0].start;
	media_set->durations += min_clip_ranges.min_clip_index;

	media_set->total_clip_count = max_clip_ranges.max_clip_index + 1 - min_clip_ranges.min_clip_index;

	// recalculate the total duration
	media_set->total_duration = 0;
	durations_end = media_set->durations + media_set->total_clip_count;
	for (durations_cur = media_set->durations; durations_cur < durations_end; durations_cur++)
	{
		media_set->total_duration += *durations_cur;
	}

	// update live params
	media_set->first_clip_time += min_clip_ranges.initial_sequence_offset + min_clip_ranges.clip_ranges[0].start;
	media_set->initial_segment_index = min_segment_index;
	media_set->initial_clip_segment_index = min_clip_ranges.first_clip_segment_index;

	if (media_set->use_discontinuity)
	{
		media_set->initial_clip_index += min_clip_ranges.min_clip_index;
	}

	if (parse_all_clips)
	{
		// parse all clips
		if (media_set->total_clip_count > MAX_CLIPS_PER_REQUEST)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_get_live_window: clip count %uD exceeds the limit per request", media_set->total_clip_count);
			return VOD_BAD_REQUEST;
		}

		clip_ranges->clip_count = media_set->total_clip_count;
		clip_ranges->max_clip_index = max_clip_ranges.max_clip_index;
	}
	else
	{
		// parse only the first clip in each sequence, assume subsequent clips have the same media info
		clip_ranges->clip_count = 1;
		clip_ranges->max_clip_index = min_clip_ranges.min_clip_index;
	}

	clip_ranges->min_clip_index = min_clip_ranges.min_clip_index;
	clip_ranges->initial_sequence_offset = 0;

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
	uint64_t segment_base_time;
	uint64_t start_time;
	uint64_t end_time;
	uint32_t* cur_duration;
	uint32_t* duration_end;
	u_char error[128];
	
	result->segmenter_conf = segmenter;
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

	result->first_clip_time = 0;
	result->initial_segment_index = 0;
	result->initial_clip_segment_index = 0;
	result->initial_clip_index = 0;

	if (params[MEDIA_SET_PARAM_DURATIONS] == NULL)
	{
		// no durations in the json -> simple vod stream
		if (request_params->clip_index != INVALID_CLIP_INDEX &&
			request_params->clip_index != 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_json: invalid clip index %uD with single clip", request_params->clip_index);
			return VOD_BAD_REQUEST;
		}

		result->total_clip_count = 1;
		result->clip_count = 1;
		result->durations = NULL;
		result->use_discontinuity = FALSE;

		// parse the sequences
		context.clip_ranges.clip_ranges = NULL;
		context.clip_ranges.clip_count = 1;
		context.clip_ranges.min_clip_index = 0;
		context.clip_ranges.max_clip_index = 0;
		context.clip_ranges.initial_sequence_offset = 0;

		context.media_set = result;
		context.base.request_context = request_context;
		context.clip_id = 1;

		return media_set_parse_sequences(&context, &params[MEDIA_SET_PARAM_SEQUENCES]->v.arr, request_params);
	}

	// vod / live
	if (params[MEDIA_SET_PARAM_PLAYLIST_TYPE] != NULL)
	{
		if (params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.len == playlist_type_vod.len &&
			vod_strncasecmp(params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.data, playlist_type_vod.data, playlist_type_vod.len) == 0)
		{
			result->type = MEDIA_SET_VOD;
		}
		else if (params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.len == playlist_type_live.len &&
			vod_strncasecmp(params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.data, playlist_type_live.data, playlist_type_live.len) == 0)
		{
			result->type = MEDIA_SET_LIVE;
		}
		else
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_json: invalid playlist type \"%V\", must be either live or vod", 
				&params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str);
			return VOD_BAD_MAPPING;
		}
	}
	else
	{
		result->type = MEDIA_SET_VOD;
	}

	// discontinuity
	if (params[MEDIA_SET_PARAM_DISCONTINUITY] != NULL)
	{
		result->use_discontinuity = params[MEDIA_SET_PARAM_DISCONTINUITY]->v.boolean;

		if (!result->use_discontinuity &&
			request_params->clip_index != INVALID_CLIP_INDEX &&
			request_params->clip_index != 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_json: clip index %uD not allowed in continuous mode", request_params->clip_index);
			return VOD_BAD_REQUEST;
		}
	}
	else
	{
		result->use_discontinuity = TRUE;
	}

	// durations
	rc = media_set_parse_durations(
		request_context,
		&params[MEDIA_SET_PARAM_DURATIONS]->v.arr,
		result);
	if (rc != VOD_OK)
	{
		return rc;
	}

	context.expected_clip_count = result->total_clip_count;

	// live params
	if (result->type == MEDIA_SET_LIVE)
	{
		rc = media_set_parse_live_params(
			request_context,
			request_params,
			segmenter,
			params,
			result, 
			&segment_base_time);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	else
	{
		segment_base_time = 0;
	}

	if (request_params->segment_index != INVALID_SEGMENT_INDEX)
	{
		// recalculate the segment index if it was determined according to timestamp
		if (request_params->segment_time != INVALID_SEGMENT_TIME)
		{
			if (request_params->segment_time < result->first_clip_time)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"media_set_parse_json: segment time %uL is smaller than first clip time %uL",
					request_params->segment_time, result->first_clip_time);
				return VOD_BAD_REQUEST;
			}

			if (result->use_discontinuity)
			{
				rc = segmenter_get_segment_index_discontinuity(
					request_context,
					segmenter,
					result->initial_segment_index,
					result->durations,
					result->total_clip_count,
					request_params->segment_time - segment_base_time,
					&request_params->segment_index);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			else
			{
				// recalculate the segment index if it was determined according to timestamp
				request_params->segment_index = segmenter_get_segment_index_no_discontinuity(
					segmenter,
					request_params->segment_time - segment_base_time);
			}
		}

		// get the segment start/end ranges
		if (result->use_discontinuity)
		{
			rc = segmenter_get_start_end_ranges_discontinuity(
				request_context,
				segmenter,
				request_params->clip_index,
				request_params->segment_index,
				result->initial_segment_index,
				result->durations,
				result->total_clip_count,
				result->total_duration,
				&context.clip_ranges);
		}
		else
		{
			start_time = result->first_clip_time - segment_base_time;
			end_time = start_time + result->total_duration;

			rc = segmenter_get_start_end_ranges_no_discontinuity(
				request_context,
				segmenter,
				request_params->segment_index,
				result->durations,
				result->total_clip_count,
				start_time,
				end_time,
				end_time,
				&context.clip_ranges);
		}

		if (rc != VOD_OK)
		{
			return rc;
		}

		result->segment_start_time = 
			result->first_clip_time + 
			context.clip_ranges.initial_sequence_offset;

		if (context.clip_ranges.clip_count > 0)
		{
			result->segment_start_time += context.clip_ranges.clip_ranges[0].start;
		}

		// in case a clip index was passed on the request, adjust the segment index 
		//	to count from the beginning of the sequence
		request_params->segment_index += context.clip_ranges.clip_index_segment_index;
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
			if (params[MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO] != NULL &&
				!params[MEDIA_SET_PARAM_CONSISTENT_SEQUENCE_MEDIA_INFO]->v.boolean)
			{
				parse_all_clips = TRUE;
			}

			if (result->type == MEDIA_SET_LIVE)
			{
				// trim the playlist to a smaller window according to the current time
				rc = media_set_get_live_window(
					request_context,
					segmenter,
					result,
					segment_base_time,
					parse_all_clips,
					&context.clip_ranges);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			else if (parse_all_clips)
			{
				// parse all clips
				if (result->total_clip_count > MAX_CLIPS_PER_REQUEST)
				{
					vod_log_error(VOD_LOG_ERR, request_context->log, 0,
						"media_set_parse_json: clip count %uD exceeds the limit per request", result->total_clip_count);
					return VOD_BAD_REQUEST;
				}

				context.clip_ranges.clip_count = result->total_clip_count;
				context.clip_ranges.min_clip_index = 0;
				context.clip_ranges.max_clip_index = result->total_clip_count - 1;
				context.clip_ranges.initial_sequence_offset = 0;
			}
			else
			{
				// parse only the first clip in each sequence, assume subsequent clips have the same media info
				context.clip_ranges.clip_count = 1;
				context.clip_ranges.min_clip_index = 0;
				context.clip_ranges.max_clip_index = 0;
				context.clip_ranges.initial_sequence_offset = 0;
			}
		}
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
