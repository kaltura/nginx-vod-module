#include "adts_encoder_filter.h"
#include "../codec_config.h"

vod_status_t 
adts_encoder_init(
	adts_encoder_state_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	const u_char* extra_data, 
	uint32_t extra_data_size)
{
	mp4a_config_t codec_config;
	vod_status_t rc;
	
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	
	if (request_context->simulation_only)
	{
		return VOD_OK;
	}

	rc = codec_config_mp4a_config_parse(request_context, extra_data, extra_data_size, &codec_config);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// Note: not parsing all the special cases in handled in ffmpeg's avpriv_mpeg4audio_get_config
	// Note: not handling pce_data
	
	vod_log_debug3(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, 
		"adts_encoder_init: object_type=%d sample_rate_index=%d channel_config=%d", 
		codec_config.object_type, codec_config.sample_rate_index, codec_config.channel_config);
	
	vod_memzero(&state->header, sizeof(state->header));
	
	adts_frame_header_set_syncword(state->header, 0xfff);
	adts_frame_header_set_protection_absent(state->header, 1);
	adts_frame_header_set_profile_object_type(state->header, codec_config.object_type - 1);
	adts_frame_header_set_sample_rate_index(state->header, codec_config.sample_rate_index);
	adts_frame_header_set_channel_configuration(state->header, codec_config.channel_config);
	adts_frame_header_set_adts_buffer_fullness(state->header, 0x7ff);
	
	return VOD_OK;
}

static vod_status_t 
adts_encoder_start_frame(void* context, output_frame_t* frame)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;
	vod_status_t rc;
	uint32_t full_frame_size;

	rc = state->next_filter->start_frame(state->next_filter_context, frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	full_frame_size	= sizeof(state->header) + frame->original_size;
	adts_frame_header_set_aac_frame_length(state->header, full_frame_size);
	
	return state->next_filter->write(state->next_filter_context, state->header, sizeof(state->header));
}

static vod_status_t 
adts_encoder_write(void* context, const u_char* buffer, uint32_t size)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;
	return state->next_filter->write(state->next_filter_context, buffer, size);
}

static vod_status_t 
adts_encoder_flush_frame(void* context, int32_t margin_size)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;
	return state->next_filter->flush_frame(state->next_filter_context, margin_size);
}

static void 
adts_encoder_simulated_write(void* context, output_frame_t* frame)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;

	frame->original_size += sizeof(state->header);

	state->next_filter->simulated_write(state->next_filter_context, frame);
}

const media_filter_t adts_encoder = {
	adts_encoder_start_frame,
	adts_encoder_write,
	adts_encoder_flush_frame,
	adts_encoder_simulated_write,
};