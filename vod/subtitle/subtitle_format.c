#include "subtitle_format.h"
#include "../media_set.h"

// typedefs
typedef struct {
	size_t size_limit;
	bool_t first_time;
	vod_str_t buffer;
} subtitle_reader_state_t;

vod_status_t
subtitle_reader_init(
	request_context_t* request_context,
	void** ctx)
{
	subtitle_reader_state_t* state;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"subtitle_reader_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->first_time = TRUE;
	state->size_limit = 2 * 1024 * 1024;			// XXXXX support configuring different metadata size limits per format

	*ctx = state;
	return VOD_OK;
}

vod_status_t
subtitle_reader_read(
	void* ctx,
	uint64_t offset,
	vod_str_t* buffer,
	media_format_read_metadata_result_t* result)
{
	subtitle_reader_state_t* state = ctx;

	if (!state->first_time)
	{
		state->buffer = *buffer;
		result->parts = &state->buffer;
		result->part_count = 1;
		return VOD_OK;
	}

	// read up to the limit
	state->first_time = FALSE;
	result->read_req.flags = MEDIA_READ_FLAG_ALLOW_EMPTY_READ;
	result->read_req.read_offset = 0;
	result->read_req.read_size = state->size_limit;
	return VOD_AGAIN;
}

vod_status_t
subtitle_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	void* context,
	uint64_t full_duration,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	subtitle_base_metadata_t* metadata;
	media_track_t* track;
	media_tags_t tags;
	uint64_t duration;

	metadata = vod_alloc(request_context->pool, sizeof(*metadata));
	if (metadata == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"subtitle_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	*result = &metadata->base;

	if (!vod_codec_in_mask(VOD_CODEC_ID_WEBVTT, parse_params->codecs_mask))
	{
		metadata->base.tracks.nelts = 0;
		return VOD_OK;
	}

	// inherit the sequence tags
	tags = parse_params->source->sequence->tags;
	if (tags.label.len == 0)
	{
		// no language, assume English
		ngx_str_set(&tags.lang_str, "eng");
		tags.language = VOD_LANG_EN;
		lang_get_native_name(tags.language, &tags.label);
	}

	// filter by language
	if (parse_params->langs_mask != NULL &&
		!vod_is_bit_set(parse_params->langs_mask, tags.language))
	{
		metadata->base.tracks.nelts = 0;
		return VOD_OK;
	}

	if (vod_array_init(&metadata->base.tracks, request_context->pool, 1, sizeof(*track)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"subtitle_parse: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	if (full_duration > parse_params->clip_from)
	{
		duration = vod_min(full_duration, parse_params->clip_to) - parse_params->clip_from;
	}
	else
	{
		duration = 0;

		if (full_duration <= 0)
		{
			full_duration = 1;			// full duration must not be empty
		}
	}

	track = vod_array_push(&metadata->base.tracks);		// can't fail
	vod_memzero(track, sizeof(*track));
	track->media_info.media_type = MEDIA_TYPE_SUBTITLE;
	track->media_info.codec_id = VOD_CODEC_ID_WEBVTT;
	track->media_info.timescale = 1000;
	track->media_info.frames_timescale = 1000;
	track->media_info.duration = duration;
	track->media_info.full_duration = full_duration;
	track->media_info.duration_millis = duration;
	track->media_info.tags = tags;
	track->media_info.bitrate = (source->len * 1000 * 8) / full_duration;

	metadata->source = *source;
	metadata->context = context;
	metadata->base.duration = duration;
	metadata->base.timescale = 1000;

	return VOD_OK;
}

