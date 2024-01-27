#include "silence_generator.h"
#include "frames_source_memory.h"
#include "../media_set_parser.h"
#include "../mp4/mp4_defs.h"
#include "../media_set.h"

#define AAC_FRAME_SAMPLES (1024)
#define AAC_SILENCE_FRAME ("\x21\x00\x49\x90\x02\x19\x00\x23\x80")
#define AAC_SILENCE_FRAME_SIZE (sizeof(AAC_SILENCE_FRAME) - 1)

vod_status_t
silence_generator_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	media_clip_source_t* source;

	source = vod_alloc(context->request_context->pool, sizeof(*source));
	if (source == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"silence_generator_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(source, sizeof(*source));

	source->base.type = MEDIA_CLIP_SILENCE_GENERATOR;

	source->sequence = context->sequence;
	source->range = context->range;
	source->clip_time = context->clip_time;
	vod_set_bit(source->tracks_mask[MEDIA_TYPE_AUDIO], 0);

	if (context->duration == UINT_MAX)
	{
		source->clip_to = ULLONG_MAX;
	}
	else
	{
		source->clip_to = context->duration;
	}

	source->next = context->generators_head;
	context->generators_head = source;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"silence_generator_parse: parsed silence source");

	*result = &source->base;

	return VOD_OK;
}

static vod_status_t
silence_generator_generate(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	media_track_array_t* result)
{
	media_sequence_t* sequence = parse_params->source->sequence;
	input_frame_t* cur_frame;
	media_track_t* track;
	media_range_t* range;
	vod_status_t rc;
	uint64_t start_time;
	uint64_t end_time;
	u_char* data;

	track = vod_alloc(request_context->pool, sizeof(*track));
	if (track == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"silence_generator_generate: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(track, sizeof(*track));

	// media info
	track->media_info.media_type = MEDIA_TYPE_AUDIO;
	track->media_info.format = FORMAT_MP4A;
	track->media_info.codec_id = VOD_CODEC_ID_AAC;
	track->media_info.bitrate = 131072;
	track->media_info.extra_data.data = (u_char*)"\x12\x10";
	track->media_info.extra_data.len = 2;
	track->media_info.u.audio.object_type_id = 0x40;
	track->media_info.u.audio.channels = 2;
	track->media_info.u.audio.channel_layout = VOD_CH_LAYOUT_STEREO;
	track->media_info.u.audio.bits_per_sample = 16;
	track->media_info.u.audio.packet_size = 0;
	track->media_info.u.audio.sample_rate = 44100;
	track->media_info.u.audio.codec_config.object_type = 2;
	track->media_info.u.audio.codec_config.sample_rate_index = 4;
	track->media_info.u.audio.codec_config.channel_config = 2;

	track->media_info.track_id = 2;
	track->media_info.timescale = track->media_info.u.audio.sample_rate;
	track->media_info.frames_timescale = track->media_info.u.audio.sample_rate;
	track->media_info.duration_millis = parse_params->clip_to - parse_params->clip_from;
	track->media_info.full_duration = (uint64_t)track->media_info.duration_millis * track->media_info.timescale;
	track->media_info.duration = track->media_info.full_duration;
	track->media_info.tags = sequence->tags;

	rc = media_format_finalize_track(
		request_context,
		parse_params->parse_type,
		&track->media_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->first_track = track;
	result->last_track = track + 1;
	result->total_track_count = 1;
	result->track_count[MEDIA_TYPE_AUDIO] = 1;

	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_ALL) == 0)
	{
		return VOD_OK;
	}

	// frames source
	rc = frames_source_memory_init(request_context, &track->frames.frames_source_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	track->frames.frames_source = &frames_source_memory;

	range = parse_params->range;
	start_time = (range->start * track->media_info.timescale) / range->timescale;
	end_time = (range->end * track->media_info.timescale) / range->timescale;

	track->first_frame_index = vod_div_ceil(start_time, AAC_FRAME_SAMPLES);
	track->frame_count = vod_div_ceil(end_time, AAC_FRAME_SAMPLES) - track->first_frame_index;

	if (track->frame_count > parse_params->max_frame_count)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"silence_generator_generate: frame count exceeds the limit %uD", parse_params->max_frame_count);
		return VOD_BAD_DATA;
	}

	track->first_frame_time_offset = (uint64_t)AAC_FRAME_SAMPLES * track->first_frame_index;
	track->total_frames_size = (uint64_t)AAC_SILENCE_FRAME_SIZE * track->frame_count;
	track->total_frames_duration = (uint64_t)AAC_FRAME_SAMPLES * track->frame_count;

	cur_frame = vod_alloc(request_context->pool, sizeof(track->frames.first_frame[0]) * track->frame_count);
	if (cur_frame == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"silence_generator_generate: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	track->frames.first_frame = cur_frame;
	track->frames.last_frame = cur_frame + track->frame_count;

	if (track->first_frame_time_offset + track->total_frames_duration >
		(uint64_t)(parse_params->clip_to - parse_params->clip_from) * track->media_info.timescale / 1000)
	{
		track->frames.clip_to = parse_params->clip_to - parse_params->clip_from;
	}
	else
	{
		track->frames.clip_to = UINT_MAX;
	}

	data = vod_alloc(request_context->pool, AAC_SILENCE_FRAME_SIZE + VOD_BUFFER_PADDING_SIZE);
	if (data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"silence_generator_generate: vod_alloc failed (3)");
		return VOD_ALLOC_FAILED;
	}

	vod_memcpy(data, AAC_SILENCE_FRAME, AAC_SILENCE_FRAME_SIZE);

	for (; cur_frame < track->frames.last_frame; cur_frame++)
	{
		cur_frame->offset = (uintptr_t)data;
		cur_frame->size = AAC_SILENCE_FRAME_SIZE;
		cur_frame->duration = AAC_FRAME_SAMPLES;
		cur_frame->key_frame = 0;
		cur_frame->pts_delay = 0;
	}

	return VOD_OK;
}

media_generator_t silence_generator = {
	VOD_CODEC_FLAG(AAC),
	{ {0}, {1}, {0} },
	silence_generator_generate
};
