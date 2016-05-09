#include "mix_filter.h"
#include "audio_filter.h"
#include "../media_set_parser.h"

// macros
#define MIX_FILTER_DESC_PATTERN "amix=inputs=%uD[%uD]"
#define INPUT_LINK_PATTERN "[%uD]"

// typedefs
typedef struct {
	media_clip_t base;
} media_clip_mix_filter_t;

// constants
static json_object_value_def_t mix_filter_params[] = {
	{ vod_string("sources"), VOD_JSON_ARRAY, offsetof(media_clip_mix_filter_t, base), media_set_parse_filter_sources },
	{ vod_null_string, 0, 0, NULL }
};

// globals
static vod_hash_t mix_filter_hash;

static uint32_t
mix_filter_get_desc_size(media_clip_t* clip)
{
	return sizeof(MIX_FILTER_DESC_PATTERN) + VOD_INT32_LEN * 2 +
		(sizeof(INPUT_LINK_PATTERN) + VOD_INT32_LEN) * clip->source_count;
}

static u_char*
mix_filter_append_desc(u_char* p, media_clip_t* clip)
{
	media_clip_t** sources_end;
	media_clip_t** sources_cur;
	uint32_t source_count = 0;

	sources_end = clip->sources + clip->source_count;
	for (sources_cur = clip->sources; sources_cur < sources_end; sources_cur++)
	{
		if (*sources_cur == NULL)
		{
			continue;
		}

		p = vod_sprintf(
			p,
			INPUT_LINK_PATTERN,
			(*sources_cur)->id);

		source_count++;
	}

	return vod_sprintf(
		p,
		MIX_FILTER_DESC_PATTERN,
		source_count,
		clip->id);
}

static audio_filter_t mix_filter = {
	mix_filter_get_desc_size,
	mix_filter_append_desc,
};

vod_status_t
mix_filter_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	media_clip_mix_filter_t* filter;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"mix_filter_parse: started");

	filter = vod_alloc(context->request_context->pool, sizeof(*filter));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mix_filter_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	filter->base.type = MEDIA_CLIP_MIX_FILTER;
	filter->base.audio_filter = &mix_filter;
	filter->base.sources = NULL;
	filter->base.source_count = 0;

	rc = vod_json_parse_object_values(
		element,
		&mix_filter_hash,
		context,
		filter);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (filter->base.source_count == 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mix_filter_parse: \"sources\" is mandatory for mix filter");
		return VOD_BAD_MAPPING;
	}

	*result = &filter->base;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"mix_filter_parse: done");

	return VOD_OK;
}

vod_status_t
mix_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"mix_filter_hash",
		mix_filter_params,
		sizeof(mix_filter_params[0]),
		&mix_filter_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
