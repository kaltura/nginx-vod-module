#include "media_set_parser.h"
#include "json_parser.h"
#include "segmenter.h"
#include "filters/gain_filter.h"
#include "filters/rate_filter.h"
#include "filters/mix_filter.h"
#include "filters/concat_clip.h"
#include "filters/dynamic_clip.h"
#include "parse_utils.h"

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
	MEDIA_SET_PARAM_REFERENCE_CLIP_INDEX,
	MEDIA_SET_PARAM_PRESENTATION_END_TIME,
	MEDIA_SET_PARAM_LIVE_SEGMENT_COUNT,

	MEDIA_SET_PARAM_COUNT
};

typedef struct {
	media_filter_parse_context_t base;
	get_clip_ranges_result_t clip_ranges;
	media_set_t* media_set;
	uint32_t clip_id;
} media_set_parse_context_t;

typedef struct {
	request_context_t* request_context;
	uint32_t expected_clip_count;
} media_set_parse_sequences_context_t;

// forward decls
static vod_status_t media_set_parse_tracks_spec(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_int32(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_int64(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_encryption_key(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_source(void* ctx, vod_json_object_t* element, void** result);
static vod_status_t media_set_parse_language(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_array(void* ctx, vod_json_value_t* value, void* dest);
static vod_status_t media_set_parse_clips_array(void* ctx, vod_json_value_t* value, void* dest);

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
	{ vod_string("concat"), concat_clip_parse },
	{ vod_string("dynamic"), dynamic_clip_parse },
	{ vod_string("source"), media_set_parse_source },
	{ vod_null_string, NULL }
};

static json_object_value_def_t media_sequence_params[] = {
	{ vod_string("id"), VOD_JSON_STRING, offsetof(media_sequence_t, id), media_set_parse_null_term_string },
	{ vod_string("clips"), VOD_JSON_ARRAY, offsetof(media_sequence_t, unparsed_clips), media_set_parse_clips_array },
	{ vod_string("language"), VOD_JSON_STRING, offsetof(media_sequence_t, language), media_set_parse_language },
	{ vod_string("label"), VOD_JSON_STRING, offsetof(media_sequence_t, label), media_set_parse_null_term_string },
	{ vod_string("firstKeyFrameOffset"), VOD_JSON_INT, offsetof(media_sequence_t, first_key_frame_offset), media_set_parse_int64 },
	{ vod_string("keyFrameDurations"), VOD_JSON_ARRAY, offsetof(media_sequence_t, key_frame_durations), media_set_parse_array },
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
	{ vod_string("referenceClipIndex"),				VOD_JSON_INT,	MEDIA_SET_PARAM_REFERENCE_CLIP_INDEX },
	{ vod_string("presentationEndTime"),			VOD_JSON_INT,	MEDIA_SET_PARAM_PRESENTATION_END_TIME },
	{ vod_string("liveSegmentCount"),				VOD_JSON_INT,	MEDIA_SET_PARAM_LIVE_SEGMENT_COUNT },
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
	vod_json_array_t* array,
	media_set_t* media_set)
{
	vod_array_part_t* part;
	uint32_t* output_cur;
	uint64_t total_duration = 0;
	int64_t cur_value;
	int64_t* cur_pos;

	if (array->count < 1 || array->count > MAX_CLIPS)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_durations: invalid number of elements in the durations array %uz", array->count);
		return VOD_BAD_MAPPING;
	}

	if (array->type != VOD_JSON_INT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_durations: invalid duration type %d expected int", array->type);
		return VOD_BAD_MAPPING;
	}

	output_cur = vod_alloc(request_context->pool, sizeof(media_set->durations[0]) * array->count);
	if (output_cur == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"media_set_parse_durations: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	media_set->durations = output_cur;

	part = &array->part;
	for (cur_pos = part->first; ; cur_pos++, output_cur++)
	{
		if ((void*)cur_pos >= part->last)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			cur_pos = part->first;
		}

		cur_value = *cur_pos;
		if (cur_value <= 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_durations: invalid duration %L must be positive", cur_value);
			return VOD_BAD_MAPPING;
		}

		if (cur_value > MAX_CLIP_DURATION)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_durations: clip duration %L too large", cur_value);
			return VOD_BAD_MAPPING;
		}

		*output_cur = cur_value;
		total_duration += cur_value;
	}

	if (total_duration > MAX_SEQUENCE_DURATION)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_durations: total duration %uL too large", total_duration);
		return VOD_BAD_MAPPING;
	}

	media_set->total_clip_count = array->count;
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
media_set_parse_int64(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	*(int64_t*)dest = value->v.num.nom;
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

vod_status_t 
media_set_parse_null_term_string(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	request_context_t* request_context = *(request_context_t**)ctx;
	vod_json_status_t rc;
	vod_str_t result;

	result.data = vod_alloc(request_context->pool, value->v.str.len + 1);
	if (result.data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"media_set_parse_null_term_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result.len = 0;

	rc = vod_json_decode_string(&result, &value->v.str);
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_null_term_string: vod_json_decode_string failed %i", rc);
		return VOD_BAD_MAPPING;
	}

	result.data[result.len] = '\0';

	*(vod_str_t*)dest = result;

	return VOD_OK;
}

static vod_status_t 
media_set_parse_language(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	request_context_t* request_context = *(request_context_t**)ctx;
	language_id_t result;

	if (value->v.str.len < LANG_ISO639_2_LEN)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_language: invalid language string length \"%V\"", &value->v.str);
		return VOD_BAD_MAPPING;
	}

	result = lang_parse_iso639_2_code(iso639_2_str_to_int(value->v.str.data));
	if (result == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_language: invalid language string \"%V\"", &value->v.str);
		return VOD_BAD_MAPPING;
	}

	*(language_id_t*)dest = result;

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
media_set_parse_array(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	*(vod_array_part_t**)dest = &value->v.arr.part;
	return VOD_OK;
}

static vod_status_t 
media_set_parse_clips_array(
	void* ctx,
	vod_json_value_t* value,
	void* dest)
{
	media_set_parse_sequences_context_t* context = ctx;
	vod_json_array_t* array = &value->v.arr;

	if (array->count != context->expected_clip_count)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_clips_array: sequence clips count %uz does not match the durations count %uD",
			array->count, context->expected_clip_count);
		return VOD_BAD_MAPPING;
	}

	if (array->type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_clips_array: invalid clip type %d expected object", array->type);
		return VOD_BAD_MAPPING;
	}

	*(vod_array_part_t**)dest = &array->part;

	return VOD_OK;
}

vod_status_t
media_set_map_source(
	request_context_t* request_context,
	u_char* string,
	media_clip_source_t* source)
{
	media_filter_parse_context_t context;
	vod_json_value_t json;
	uint32_t duration = source->clip_to - source->clip_from;
	u_char error[128];
	vod_status_t rc;

	rc = vod_json_parse(request_context->pool, string, &json, error, sizeof(error));
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_map_source: failed to parse json %i: %s", rc, error);
		return VOD_BAD_MAPPING;
	}

	if (json.type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_map_source: invalid root element type %d expected object", json.type);
		return VOD_BAD_MAPPING;
	}

	context.request_context = request_context;

	source->mapped_uri.len = (size_t)-1;

	rc = vod_json_parse_object_values(&json.v.obj, &media_clip_source_hash, &context, source);
	if (rc != VOD_OK)
	{
		return rc;
	}

	switch (source->mapped_uri.len)
	{
	case (size_t)-1:
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_map_source: missing path in source object");
		return VOD_BAD_MAPPING;

	case 0:
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_map_source: empty path in source object");
		return VOD_NOT_FOUND;
	}

	source->clip_to = source->clip_from + duration;
	source->stripped_uri = source->mapped_uri;

	return VOD_OK;
}

static vod_status_t
media_set_parse_source(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	media_set_parse_context_t* context = ctx;
	media_clip_source_t* source;
	vod_status_t rc;

	source = vod_alloc(context->base.request_context->pool, sizeof(*source));
	if (source == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_source: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

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
	source->stripped_uri = source->mapped_uri;

	source->next = context->base.sources_head;
	context->base.sources_head = source;

	vod_log_debug4(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
		"media_set_parse_source: parsed clip source - path=%V tracks[v]=0x%uxD tracks[a]=0x%uxD, clipFrom=%uD", 
		&source->mapped_uri, 
		source->tracks_mask[MEDIA_TYPE_VIDEO],
		source->tracks_mask[MEDIA_TYPE_AUDIO],
		source->clip_from);

	*result = &source->base;

	return VOD_OK;
}

vod_status_t
media_set_parse_clip(
	void* ctx,
	vod_json_object_t* element,
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
	vod_array_part_t* part;
	vod_json_object_t* sources_cur;
	vod_json_array_t* sources = &value->v.arr;
	media_clip_t** output;
	media_clip_t* filter = dest;
	vod_status_t rc;

	if (sources->count < 1 || sources->count > MAX_SOURCES)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_filter_sources: invalid number of elements in the sources array %uz", sources->count);
		return VOD_BAD_MAPPING;
	}

	if (sources->type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"media_set_parse_filter_sources: invalid source type %d expected object", sources->type);
		return VOD_BAD_MAPPING;
	}

	filter->source_count = sources->count;
	filter->sources = vod_alloc(context->request_context->pool, sizeof(filter->sources[0]) * filter->source_count);
	if (filter->sources == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"media_set_parse_filter_sources: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	part = &sources->part;
	for (sources_cur = part->first, output = filter->sources; 
		; 
		sources_cur++, output++)
	{
		if ((void*)sources_cur >= part->last)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			sources_cur = part->first;
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
media_set_parse_sequences(
	request_context_t* request_context,
	media_set_t* media_set,
	vod_json_array_t* array, 
	request_params_t* request_params)
{
	media_set_parse_sequences_context_t context;
	vod_array_part_t* part;
	vod_json_object_t* cur_pos;
	media_sequence_t* cur_output;
	vod_status_t rc;
	uint32_t required_sequences_num;
	uint32_t index;

	if (array->count < 1 || array->count > MAX_SEQUENCES)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_sequences: invalid number of sequences %uz", array->count);
		return VOD_BAD_MAPPING;
	}

	if (array->type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_sequences: invalid sequence type %d expected object", array->type);
		return VOD_BAD_MAPPING;
	}

	required_sequences_num = vod_get_number_of_set_bits(request_params->sequences_mask);
	required_sequences_num = vod_min(array->count, required_sequences_num);

	cur_output = vod_alloc(
		request_context->pool, 
		sizeof(media_set->sequences[0]) * required_sequences_num);
	if (cur_output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"media_set_parse_sequences: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	media_set->sequences = cur_output;

	context.request_context = request_context;
	context.expected_clip_count = media_set->total_clip_count;

	index = 0;
	part = &array->part;
	for (cur_pos = part->first; ; cur_pos++, index++)
	{
		if ((void*)cur_pos >= part->last)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			cur_pos = part->first;
		}

		if ((request_params->sequences_mask & (1 << index)) == 0)
		{
			continue;
		}

		cur_output->id.len = 0;
		cur_output->unparsed_clips = NULL;
		cur_output->language = 0;
		cur_output->label.len = 0;
		cur_output->first_key_frame_offset = 0;
		cur_output->key_frame_durations = NULL;

		rc = vod_json_parse_object_values(
			cur_pos,
			&media_sequence_hash,
			&context,
			cur_output);
		if (rc != VOD_OK)
		{
			return rc;
		}
		
		if (cur_output->unparsed_clips == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_set_parse_sequences: missing clips in sequence object");
			return VOD_BAD_MAPPING;
		}

		if (request_params->sequence_id.len != 0 &&
			(cur_output->id.len != request_params->sequence_id.len ||
			vod_memcmp(cur_output->id.data, request_params->sequence_id.data, cur_output->id.len) != 0))
		{
			continue;
		}

		if (cur_output->language != 0 && cur_output->label.len == 0)
		{
			lang_get_native_name(cur_output->language, &cur_output->label);
		}

		cur_output->index = index;
		cur_output->mapped_uri.len = 0;
		cur_output->stripped_uri.len = 0;
		cur_output++;
	}

	media_set->sequence_count = cur_output - media_set->sequences;
	if (media_set->sequence_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_sequences: request has no sequences");
		return VOD_BAD_REQUEST;
	}

	media_set->sequences_end = cur_output;
	media_set->has_multi_sequences = array->count > 1;

	return VOD_OK;
}

static vod_status_t 
media_set_parse_sequence_clips(
	media_set_parse_context_t* context,
	vod_array_part_t* part,
	media_clip_t*** clips)
{
	vod_json_object_t* cur_pos;
	media_clip_t** output_cur;
	media_clip_t** output_end;
	vod_status_t rc;
	uint32_t* cur_duration;
	uint32_t index;

	output_cur = vod_alloc(context->base.request_context->pool, sizeof(output_cur[0]) * context->clip_ranges.clip_count);
	if (output_cur == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->base.request_context->log, 0,
			"media_set_parse_sequence_clips: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	output_end = output_cur + context->clip_ranges.clip_count;
	*clips = output_cur;

	context->base.sequence_offset = context->clip_ranges.initial_sequence_offset;
	context->base.range = context->clip_ranges.clip_ranges;
	cur_duration = context->media_set->durations + context->clip_ranges.min_clip_index;

	// find the first element
	index = context->clip_ranges.min_clip_index;
	while (index >= part->count)
	{
		index -= part->count;
		part = part->next;
	}

	for (cur_pos = (vod_json_object_t*)part->first + index; 
		output_cur < output_end; 
		output_cur++, cur_pos++)
	{
		if ((void*)cur_pos >= part->last)
		{
			part = part->next;
			cur_pos = part->first;
		}

		context->base.duration = cur_duration != NULL ? *cur_duration : UINT_MAX;

		rc = media_set_parse_clip(
			context, 
			cur_pos, 
			NULL, 
			output_cur);
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
		cur_duration++;
	}

	return VOD_OK;
}

static vod_status_t
media_set_parse_sequences_clips(
	media_set_parse_context_t* context)
{
	media_sequence_t* sequence;
	media_set_t* media_set = context->media_set;
	vod_status_t rc;

	context->base.sources_head = NULL;
	context->base.mapped_sources_head = NULL;
	context->base.dynamic_clips_head = NULL;

	for (sequence = media_set->sequences; sequence < media_set->sequences_end; sequence++)
	{
		context->base.sequence = sequence;

		rc = media_set_parse_sequence_clips(
			context,
			sequence->unparsed_clips,
			&sequence->clips);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	media_set->sources_head = context->base.sources_head;
	media_set->mapped_sources_head = context->base.mapped_sources_head;
	media_set->dynamic_clips_head = context->base.dynamic_clips_head;

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

	rc = concat_clip_parser_init(pool, temp_pool);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = dynamic_clip_parser_init(pool, temp_pool);
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
	int64_t live_segment_count,
	uint64_t segment_base_time,
	bool_t parse_all_clips,
	get_clip_ranges_result_t* clip_ranges)
{
	get_clip_ranges_result_t max_clip_ranges;
	get_clip_ranges_result_t min_clip_ranges;
	get_clip_ranges_params_t get_ranges_params;
	vod_status_t rc;
	uint64_t current_time;
	uint32_t* durations_end;
	uint32_t* durations_cur;
	uint32_t min_segment_index;
	uint32_t max_segment_index;

	// non-segment request

	if (live_segment_count <= 0)
	{
		// find the full range of segments included in the mapping
		if (media_set->use_discontinuity)
		{
			min_segment_index = media_set->initial_segment_index;

			rc = segmenter_get_segment_index_discontinuity(
				request_context,
				segmenter,
				media_set->initial_segment_index,
				media_set->durations,
				media_set->total_clip_count,
				media_set->total_duration - 1,
				&max_segment_index);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		else
		{
			min_segment_index = segmenter_get_segment_index_no_discontinuity_round_up(
				segmenter,
				media_set->first_clip_time - segment_base_time);

			max_segment_index = segmenter_get_segment_index_no_discontinuity(
				segmenter,
				media_set->first_clip_time + media_set->total_duration - segment_base_time - 1);

			if (!media_set->presentation_end)
			{
				max_segment_index--;
			}
		}

		// if live segment count is negative, output the last N segments
		if (live_segment_count < 0 && 
			max_segment_index - min_segment_index + 1 > (uintptr_t)(-live_segment_count))
		{
			min_segment_index = max_segment_index + (int32_t)(live_segment_count + 1);
		}
	}
	else
	{
		// output live_segment_count segments (at maximum) according to the current time
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
		if (max_segment_index > media_set->initial_segment_index + live_segment_count - 1)
		{
			min_segment_index = max_segment_index - live_segment_count + 1;
		}
		else
		{
			min_segment_index = media_set->initial_segment_index;
		}
	}

	// get the ranges of the first and last segments
	get_ranges_params.request_context = request_context;
	get_ranges_params.conf = segmenter;
	get_ranges_params.segment_index = min_segment_index;
	get_ranges_params.clip_durations = media_set->durations;
	get_ranges_params.total_clip_count = media_set->total_clip_count;
	get_ranges_params.first_key_frame_offset = media_set->sequences[0].first_key_frame_offset;
	get_ranges_params.key_frame_durations = media_set->sequences[0].key_frame_durations;

	if (media_set->use_discontinuity)
	{
		get_ranges_params.clip_index = 0;
		get_ranges_params.initial_segment_index = media_set->initial_segment_index;

		// TODO: consider optimizing this by creating dedicated segmenter functions
		rc = segmenter_get_start_end_ranges_discontinuity(
			&get_ranges_params,
			&min_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}

		get_ranges_params.segment_index = max_segment_index;

		rc = segmenter_get_start_end_ranges_discontinuity(
			&get_ranges_params,
			&max_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	else
	{
		get_ranges_params.start_time = media_set->first_clip_time - segment_base_time;
		get_ranges_params.end_time = get_ranges_params.start_time + media_set->total_duration;
		get_ranges_params.last_segment_end = get_ranges_params.end_time;

		rc = segmenter_get_start_end_ranges_no_discontinuity(
			&get_ranges_params,
			&min_clip_ranges);
		if (rc != VOD_OK)
		{
			return rc;
		}

		get_ranges_params.segment_index = max_segment_index;

		rc = segmenter_get_start_end_ranges_no_discontinuity(
			&get_ranges_params,
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

static int64_t
media_set_apply_live_segment_count_param(
	int64_t live_segment_count, 
	int64_t live_segment_count_param)
{
	// ignore the json param if it has a different sign or greater absolute value than the conf
	if (live_segment_count > 0)
	{
		if (live_segment_count_param > 0 &&
			live_segment_count_param < live_segment_count)
		{
			return live_segment_count_param;
		}
	}
	else if (live_segment_count < 0)
	{
		if (live_segment_count_param < 0 &&
			live_segment_count_param > live_segment_count)
		{
			return live_segment_count_param;
		}
	}
	else
	{
		if (live_segment_count_param < 0)
		{
			return live_segment_count_param;
		}
	}

	return live_segment_count;
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
	get_clip_ranges_params_t get_ranges_params;
	vod_json_value_t* params[MEDIA_SET_PARAM_COUNT];
	vod_json_value_t json;
	vod_status_t rc;
	uint64_t segment_base_time;
	int64_t live_segment_count;
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

	if (json.type != VOD_JSON_OBJECT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_json: invalid root element type %d expected object", json.type);
		return VOD_BAD_MAPPING;
	}

	vod_memzero(params, sizeof(params));

	vod_json_get_object_values(
		&json.v.obj,
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
		result->type = MEDIA_SET_VOD;
		result->presentation_end = TRUE;

		// parse the sequences
		rc = media_set_parse_sequences(
			request_context,
			result,
			&params[MEDIA_SET_PARAM_SEQUENCES]->v.arr,
			request_params);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// parse the clips
		context.clip_ranges.clip_ranges = NULL;
		context.clip_ranges.clip_count = 1;
		context.clip_ranges.min_clip_index = 0;
		context.clip_ranges.max_clip_index = 0;
		context.clip_ranges.initial_sequence_offset = 0;

		context.media_set = result;
		context.base.request_context = request_context;
		context.clip_id = 1;

		rc = media_set_parse_sequences_clips(&context);
		if (rc != VOD_OK)
		{
			return rc;
		}

		return rc;
	}

	// vod / live
	if (params[MEDIA_SET_PARAM_PLAYLIST_TYPE] == NULL || 
		(params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.len == playlist_type_vod.len &&
		vod_strncasecmp(params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.data, playlist_type_vod.data, playlist_type_vod.len) == 0))
	{
		result->type = MEDIA_SET_VOD;
		result->presentation_end = TRUE;
	}
	else if (params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.len == playlist_type_live.len &&
		vod_strncasecmp(params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str.data, playlist_type_live.data, playlist_type_live.len) == 0)
	{
		result->type = MEDIA_SET_LIVE;
		result->presentation_end = params[MEDIA_SET_PARAM_PRESENTATION_END_TIME] != NULL &&
			params[MEDIA_SET_PARAM_PRESENTATION_END_TIME]->v.num.nom <= vod_time() * 1000;
	}
	else
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"media_set_parse_json: invalid playlist type \"%V\", must be either live or vod", 
			&params[MEDIA_SET_PARAM_PLAYLIST_TYPE]->v.str);
		return VOD_BAD_MAPPING;
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

	// sequences
	rc = media_set_parse_sequences(
		request_context,
		result,
		&params[MEDIA_SET_PARAM_SEQUENCES]->v.arr,
		request_params);
	if (rc != VOD_OK)
	{
		return rc;
	}

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
					request_params->segment_time - segment_base_time + SEGMENT_FROM_TIMESTAMP_MARGIN,
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
					request_params->segment_time - segment_base_time + SEGMENT_FROM_TIMESTAMP_MARGIN);
			}
		}

		// get the segment start/end ranges
		get_ranges_params.request_context = request_context;
		get_ranges_params.conf = segmenter;
		get_ranges_params.segment_index = request_params->segment_index;
		get_ranges_params.clip_durations = result->durations;
		get_ranges_params.total_clip_count = result->total_clip_count;
		get_ranges_params.first_key_frame_offset = result->sequences[0].first_key_frame_offset;
		get_ranges_params.key_frame_durations = result->sequences[0].key_frame_durations;

		if (result->use_discontinuity)
		{
			get_ranges_params.clip_index = request_params->clip_index;
			get_ranges_params.initial_segment_index = result->initial_segment_index;

			rc = segmenter_get_start_end_ranges_discontinuity(
				&get_ranges_params,
				&context.clip_ranges);
		}
		else
		{
			get_ranges_params.start_time = result->first_clip_time - segment_base_time;
			get_ranges_params.end_time = get_ranges_params.start_time + result->total_duration;
			get_ranges_params.last_segment_end = get_ranges_params.end_time;

			rc = segmenter_get_start_end_ranges_no_discontinuity(
				&get_ranges_params,
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
				// trim the playlist to a smaller window if needed
				live_segment_count = segmenter->live_segment_count;

				if (params[MEDIA_SET_PARAM_LIVE_SEGMENT_COUNT] != NULL)
				{
					live_segment_count = media_set_apply_live_segment_count_param(
						live_segment_count,
						params[MEDIA_SET_PARAM_LIVE_SEGMENT_COUNT]->v.num.nom);
				}

				rc = media_set_get_live_window(
					request_context,
					segmenter,
					result,
					live_segment_count,
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
				if (params[MEDIA_SET_PARAM_REFERENCE_CLIP_INDEX] != NULL)
				{
					context.clip_ranges.min_clip_index = params[MEDIA_SET_PARAM_REFERENCE_CLIP_INDEX]->v.num.nom - 1 - result->initial_clip_index;
					if (context.clip_ranges.min_clip_index >= result->total_clip_count)
					{
						vod_log_error(VOD_LOG_ERR, request_context->log, 0,
							"media_set_parse_json: reference clip index %uD exceeds the total number of clips %uD", 
							context.clip_ranges.min_clip_index, result->total_clip_count);
						return VOD_BAD_MAPPING;
					}

					context.clip_ranges.max_clip_index = context.clip_ranges.min_clip_index;
				}
				else
				{
					context.clip_ranges.min_clip_index = 0;
					context.clip_ranges.max_clip_index = 0;
				}

				// parse only the first clip in each sequence, assume subsequent clips have the same media info
				context.clip_ranges.clip_count = 1;
				context.clip_ranges.initial_sequence_offset = 0;
			}
		}
	}

	result->clip_count = context.clip_ranges.clip_count;

	// sequences
	context.media_set = result;
	context.base.request_context = request_context;
	context.clip_id = 1;

	rc = media_set_parse_sequences_clips(&context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
