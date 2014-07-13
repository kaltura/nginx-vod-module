#include "adts_encoder_filter.h"
#include "bit_read_stream.h"

#define AOT_ESCAPE (31)

vod_status_t 
adts_encoder_init(
	adts_encoder_state_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	const u_char* extra_data, 
	uint32_t extra_data_size)
{
	bit_reader_state_t reader;
	int object_type;
	int sample_rate_index;
	int channel_config;
	
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	
	if (request_context->simulation_only)
	{
		return VOD_OK;
	}

	vod_log_buffer(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "adts_encoder_init: extra data ", extra_data, extra_data_size);
	
	init_bits_reader(&reader, extra_data, extra_data_size);

    object_type = get_bits(&reader, 5);
    if (object_type == AOT_ESCAPE)
        object_type = 32 + get_bits(&reader, 6);
	
	sample_rate_index = get_bits(&reader, 4);
	if (sample_rate_index == 0x0f)
		get_bits(&reader, 24);

    channel_config = get_bits(&reader, 4);
	
	if (reader.stream.eof_reached)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0, 
			"adts_encoder_init: failed to read all required audio extra data fields");
		return VOD_BAD_DATA;
	}
	
	// Note: not parsing all the special cases in handled in ffmpeg's avpriv_mpeg4audio_get_config
	// Note: not handling pce_data
	
	vod_log_debug3(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, 
		"adts_encoder_init: object_type=%d sample_rate_index=%d channel_config=%d", 
		object_type, sample_rate_index, channel_config);
	
	vod_memzero(&state->header, sizeof(state->header));
	
	adts_frame_header_set_syncword(state->header, 0xfff);
	adts_frame_header_set_protection_absent(state->header, 1);
	adts_frame_header_set_profile_object_type(state->header, object_type - 1);
	adts_frame_header_set_sample_rate_index(state->header, sample_rate_index);
	adts_frame_header_set_channel_configuration(state->header, channel_config);
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