#include "adts_encoder_filter.h"
#include "sample_aes_aac_filter.h"
#include "../codec_config.h"

vod_status_t 
adts_encoder_init(
	adts_encoder_state_t* state, 
	request_context_t* request_context,
	hls_encryption_params_t* encryption_params,
	const media_filter_t* next_filter,
	void* next_filter_context)
{
	vod_status_t rc;
	
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	state->request_context = request_context;
	
	if (request_context->simulation_only)
	{
		return VOD_OK;
	}

	if (encryption_params->type == HLS_ENC_SAMPLE_AES)
	{
		rc = sample_aes_aac_filter_init(
			&state->sample_aes_context, 
			request_context, 
			next_filter->write, 
			next_filter_context, 
			encryption_params->key, 
			encryption_params->iv);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->body_write = sample_aes_aac_filter_write_frame_body;
		state->body_write_context = state->sample_aes_context;
	}
	else
	{
		state->sample_aes_context = NULL;

		state->body_write = next_filter->write;
		state->body_write_context = next_filter_context;
	}
	
	return VOD_OK;
}

vod_status_t
adts_encoder_set_media_info(
	adts_encoder_state_t* state,
	media_info_t* media_info)
{
	mp4a_config_t* codec_config = &media_info->u.audio.codec_config;

	if (state->request_context->simulation_only)
	{
		return VOD_OK;
	}

	// Note: not parsing all the special cases handled in ffmpeg's avpriv_mpeg4audio_get_config
	// Note: not handling pce_data

	vod_memzero(&state->header, sizeof(state->header));

	adts_frame_header_set_syncword(state->header, 0xfff);
	adts_frame_header_set_protection_absent(state->header, 1);
	adts_frame_header_set_profile_object_type(state->header, codec_config->object_type - 1);
	adts_frame_header_set_sample_rate_index(state->header, codec_config->sample_rate_index);
	adts_frame_header_set_channel_configuration(state->header, codec_config->channel_config);
	adts_frame_header_set_adts_buffer_fullness(state->header, 0x7ff);

	return VOD_OK;
}

static vod_status_t 
adts_encoder_start_frame(void* context, output_frame_t* frame)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;
	vod_status_t rc;

	if (state->sample_aes_context != NULL)
	{
		rc = sample_aes_aac_start_frame(state->sample_aes_context, frame);		// passing the frame data size (excl. header)
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	frame->size += sizeof(state->header);
	frame->header_size += 1;
	
	rc = state->next_filter->start_frame(state->next_filter_context, frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	adts_frame_header_set_aac_frame_length(state->header, frame->size);
	
	return state->next_filter->write(state->next_filter_context, state->header, sizeof(state->header));
}

static vod_status_t 
adts_encoder_write(void* context, const u_char* buffer, uint32_t size)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;

	return state->body_write(state->body_write_context, buffer, size);
}

static vod_status_t 
adts_encoder_flush_frame(void* context, bool_t last_stream_frame)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;

	return state->next_filter->flush_frame(state->next_filter_context, last_stream_frame);
}


static void 
adts_encoder_simulated_start_frame(void* context, output_frame_t* frame)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;

	frame->header_size += 1;

	state->next_filter->simulated_start_frame(state->next_filter_context, frame);
	state->next_filter->simulated_write(state->next_filter_context, sizeof(state->header));
}

static void 
adts_encoder_simulated_write(void* context, uint32_t size)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;

	state->next_filter->simulated_write(state->next_filter_context, size);
}

static void 
adts_encoder_simulated_flush_frame(void* context, bool_t last_stream_frame)
{
	adts_encoder_state_t* state = (adts_encoder_state_t*)context;

	state->next_filter->simulated_flush_frame(state->next_filter_context, last_stream_frame);
}


const media_filter_t adts_encoder = {
	adts_encoder_start_frame,
	adts_encoder_write,
	adts_encoder_flush_frame,
	adts_encoder_simulated_start_frame,
	adts_encoder_simulated_write,
	adts_encoder_simulated_flush_frame,
};