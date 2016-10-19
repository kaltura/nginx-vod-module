#include "gain_filter.h"
#include "audio_filter.h"
#include "../media_set_parser.h"

// macros
#define GAIN_FILTER_DESC_PATTERN "[%uD]volume=volume=%uD.%02uD[%uD]"

// enums
enum {
	GAIN_FILTER_PARAM_GAIN,
	GAIN_FILTER_PARAM_SOURCE,

	GAIN_FILTER_PARAM_COUNT
};

// typedefs
typedef struct {
	media_clip_t base;
	vod_fraction_t gain;
} media_clip_gain_filter_t;

// constants
static json_object_key_def_t gain_filter_params[] = {
	{ vod_string("gain"), VOD_JSON_FRAC, GAIN_FILTER_PARAM_GAIN },
	{ vod_string("source"), VOD_JSON_OBJECT, GAIN_FILTER_PARAM_SOURCE },
	{ vod_null_string, 0, 0 }
};

// globals
static vod_hash_t gain_filter_hash;

static uint32_t
gain_filter_get_desc_size(media_clip_t* clip)
{
	return sizeof(GAIN_FILTER_DESC_PATTERN) + VOD_INT32_LEN * 4;
}

static u_char*
gain_filter_append_desc(u_char* p, media_clip_t* clip)
{
	media_clip_gain_filter_t* filter = vod_container_of(clip, media_clip_gain_filter_t, base);
	uint32_t denom;
	uint32_t num;

	// normalize the fraction to 100 denom
	num = filter->gain.num;
	denom = filter->gain.denom;
	while (denom < 100)
	{
		num *= 10;
		denom *= 10;
	}

	return vod_sprintf(
		p,
		GAIN_FILTER_DESC_PATTERN,
		clip->sources[0]->id,
		num / 100,
		num % 100,
		clip->id);
}

static audio_filter_t gain_filter = {
	gain_filter_get_desc_size,
	gain_filter_append_desc,
};

vod_status_t
gain_filter_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	media_clip_gain_filter_t* filter;
	vod_json_value_t* params[GAIN_FILTER_PARAM_COUNT];
	vod_json_value_t* source;
	vod_json_value_t* gain;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"gain_filter_parse: started");

	vod_memzero(params, sizeof(params));

	vod_json_get_object_values(
		element,
		&gain_filter_hash,
		params);

	gain = params[GAIN_FILTER_PARAM_GAIN];
	source = params[GAIN_FILTER_PARAM_SOURCE];

	if (gain == NULL || source == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"gain_filter_parse: \"gain\" and \"source\" are mandatory for gain filter");
		return VOD_BAD_MAPPING;
	}

	if (gain->v.num.num <= 0 || gain->v.num.denom > 100)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"gain_filter_parse: invalid gain %L/%uL, expecting a positive number with up to 2 decimal points", 
			gain->v.num.num, gain->v.num.denom);
		return VOD_BAD_MAPPING;
	}

	filter = vod_alloc(context->request_context->pool, sizeof(*filter) + sizeof(filter->base.sources[0]));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"gain_filter_parse: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}
	filter->base.sources = (void*)(filter + 1);
	filter->base.source_count = 1;

	filter->base.type = MEDIA_CLIP_GAIN_FILTER;
	filter->base.audio_filter = &gain_filter;
	filter->gain.num = gain->v.num.num;
	filter->gain.denom = gain->v.num.denom;

	rc = media_set_parse_clip(
		context,
		&source->v.obj,
		&filter->base,
		&filter->base.sources[0]);
	if (rc != VOD_JSON_OK)
	{
		return rc;
	}

	*result = &filter->base;

	vod_log_debug2(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"gain_filter_parse: done, gain=%uD/%uD", filter->gain.num, filter->gain.denom);

	return VOD_OK;
}

vod_status_t
gain_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"gain_filter_hash",
		gain_filter_params,
		sizeof(gain_filter_params[0]),
		&gain_filter_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
