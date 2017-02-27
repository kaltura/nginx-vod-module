#include "adts_encoder_filter.h"
#include "../codec_config.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_ADTS)
#define get_context(ctx) ((adts_encoder_state_t*)ctx->context[THIS_FILTER])

// typedefs
typedef struct {
	// input data
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;
	media_filter_simulated_start_frame_t simulated_start_frame;
	media_filter_simulated_write_t simulated_write;

	// fixed
	u_char header[sizeof_adts_frame_header];
} adts_encoder_state_t;

vod_status_t
adts_encoder_set_media_info(
	media_filter_context_t* context,
	media_info_t* media_info)
{
	adts_encoder_state_t* state = get_context(context);
	mp4a_config_t* codec_config = &media_info->u.audio.codec_config;

	if (context->request_context->simulation_only)
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
adts_encoder_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	adts_encoder_state_t* state = get_context(context);
	vod_status_t rc;

	frame->size += sizeof(state->header);
	frame->header_size += 1;
	
	rc = state->start_frame(context, frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	adts_frame_header_set_aac_frame_length(state->header, frame->size);
	
	return state->write(context, state->header, sizeof(state->header));
}


static void 
adts_encoder_simulated_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	adts_encoder_state_t* state = get_context(context);

	frame->header_size += 1;

	state->simulated_start_frame(context, frame);
	state->simulated_write(context, sizeof(state->header));
}

vod_status_t
adts_encoder_init(
	media_filter_t* filter,
	media_filter_context_t* context)
{
	adts_encoder_state_t* state;
	request_context_t* request_context = context->request_context;

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"adts_encoder_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// save required functions
	state->start_frame = filter->start_frame;
	state->write = filter->write;
	state->simulated_start_frame = filter->simulated_start_frame;
	state->simulated_write = filter->simulated_write;

	// override functions
	filter->start_frame = adts_encoder_start_frame;
	filter->simulated_start_frame = adts_encoder_simulated_start_frame;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}
