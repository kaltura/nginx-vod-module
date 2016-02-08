#include "mp4_to_annexb_filter.h"
#include "sample_aes_avc_filter.h"
#include "../read_stream.h"
#include "../avc_defs.h"

// states
enum {
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_COPY_PACKET,
	STATE_SKIP_PACKET,
};

// constants
static const u_char avc_aud_nal_packet[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };	// f = all pic types + stop bit
static const u_char hevc_aud_nal_packet[] = { 0x00, 0x00, 0x00, 0x01, 0x46, 0xf0 };	// f = all pic types + stop bit
static const u_char nal_marker[] = { 0x00, 0x00, 0x00, 0x01 };
static const u_char zero_padding[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

vod_status_t 
mp4_to_annexb_init(
	mp4_to_annexb_state_t* state, 
	request_context_t* request_context,
	hls_encryption_params_t* encryption_params,
	const media_filter_t* next_filter,
	void* next_filter_context)
{
	vod_status_t rc;

	if (encryption_params->type == HLS_ENC_SAMPLE_AES)
	{
		rc = sample_aes_avc_filter_init(
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

		state->body_write = sample_aes_avc_filter_write_nal_body;
		state->body_write_context = state->sample_aes_context;
	}
	else
	{
		state->sample_aes_context = NULL;

		state->body_write = next_filter->write;
		state->body_write_context = next_filter_context;
	}

	state->request_context = request_context;
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;

	return VOD_OK;
}

vod_status_t
mp4_to_annexb_set_media_info(
	mp4_to_annexb_state_t* state, 
	media_info_t* media_info)
{
	state->nal_packet_size_length = media_info->u.video.nal_packet_size_length;
	if (state->nal_packet_size_length < 1 || state->nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_to_annexb_set_media_info: invalid nal packet size length %uD", state->nal_packet_size_length);
		return VOD_BAD_DATA;
	}

	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_HEVC:
		if (state->sample_aes_context != NULL)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mp4_to_annexb_set_media_info: hevc with sample-aes is not supported");
			return VOD_BAD_REQUEST;
		}

		state->unit_type_mask = (0x3F << 1);
		state->aud_unit_type = (HEVC_NAL_AUD << 1);
		state->aud_nal_packet = hevc_aud_nal_packet;
		state->aud_nal_packet_size = sizeof(hevc_aud_nal_packet);
		break;

	default:		// AVC
		state->unit_type_mask = 0x1F;
		state->aud_unit_type = AVC_NAL_AUD;
		state->aud_nal_packet = avc_aud_nal_packet;
		state->aud_nal_packet_size = sizeof(avc_aud_nal_packet);
		break;
	}

	state->extra_data = media_info->extra_data.data;
	state->extra_data_size = media_info->extra_data.len;

	return VOD_OK;
}

bool_t 
mp4_to_annexb_simulation_supported(media_info_t* media_info)
{
	/* When the packet size field length is 4 we can bound the output size - since every 4-byte length
		field is transformed to a \0\0\0\x01 or \0\0\x01 NAL marker, the output size is <= the input size.
		When the packet size field length is less than 4, the output size may be greater than input size by
		the number of NAL packets. Since we don't know this number in advance we have no way to bound the
		output size. Luckily, ffmpeg always uses 4 byte size fields - see ff_isom_write_avcc */
	if (media_info->u.video.nal_packet_size_length != 4)
	{
		return FALSE;
	}

	return TRUE;
}

static vod_status_t 
mp4_to_annexb_start_frame(void* context, output_frame_t* frame)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;
	vod_status_t rc;

	state->frame_size_left = frame->size;		// not counting the AUD or extra data since they are written here

	frame->size += state->aud_nal_packet_size;
	frame->header_size += state->aud_nal_packet_size;
	if (frame->key)
	{
		frame->size += state->extra_data_size;
	}

	rc = state->next_filter->start_frame(state->next_filter_context, frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	// init state
	state->first_frame_packet = TRUE;
	state->cur_state = STATE_PACKET_SIZE;
	state->length_bytes_left = state->nal_packet_size_length;
	state->packet_size_left = 0;

	// write access unit delimiter packet
	rc = state->next_filter->write(state->next_filter_context, state->aud_nal_packet, state->aud_nal_packet_size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (frame->key)
	{
		rc = state->next_filter->write(state->next_filter_context, state->extra_data, state->extra_data_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t 
mp4_to_annexb_write(void* context, const u_char* buffer, uint32_t size)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;
	const u_char* buffer_end = buffer + size;
	uint32_t write_size;
	int unit_type;
	vod_status_t rc;

	while (buffer < buffer_end)
	{
		switch (state->cur_state)
		{
		case STATE_PACKET_SIZE:
			for (; state->length_bytes_left && buffer < buffer_end; state->length_bytes_left--)
			{
				state->packet_size_left = (state->packet_size_left << 8) | *buffer++;
			}
			if (buffer >= buffer_end)
			{
				break;
			}
			state->cur_state++;
			// fall through
			
		case STATE_NAL_TYPE:
			unit_type = *buffer & state->unit_type_mask;
			if (unit_type == state->aud_unit_type)
			{
				state->cur_state = STATE_SKIP_PACKET;
				break;
			}

			if (state->sample_aes_context != NULL)
			{
				rc = sample_aes_avc_start_nal_unit(state->sample_aes_context, unit_type, state->packet_size_left);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
						
			if (state->first_frame_packet)
			{
				state->first_frame_packet = FALSE;
				state->frame_size_left -= sizeof(nal_marker);
				rc = state->next_filter->write(state->next_filter_context, nal_marker, sizeof(nal_marker));
			}
			else
			{
				state->frame_size_left -= (sizeof(nal_marker) - 1);
				rc = state->next_filter->write(state->next_filter_context, nal_marker + 1, sizeof(nal_marker) - 1);
			}
			
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			state->cur_state++;
			// fall through
			
		case STATE_COPY_PACKET:
		case STATE_SKIP_PACKET:
			write_size = vod_min(state->packet_size_left, (uint32_t)(buffer_end - buffer));
			if (state->cur_state == STATE_COPY_PACKET)
			{
				state->frame_size_left -= write_size;
				rc = state->body_write(state->body_write_context, buffer, write_size);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
			buffer += write_size;
			state->packet_size_left -= write_size;
			if (state->packet_size_left <= 0)
			{
				state->cur_state = STATE_PACKET_SIZE;
				state->length_bytes_left = state->nal_packet_size_length;
				state->packet_size_left = 0;
			}
			break;
		}
	}
	
	return VOD_OK;
}

static vod_status_t 
mp4_to_annexb_flush_frame(void* context, bool_t last_stream_frame)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;
	vod_status_t rc;
	int32_t cur_size;

	if (state->nal_packet_size_length == 4)
	{
		if (state->frame_size_left < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mp4_to_annexb_flush_frame: frame exceeded the calculated size by %D bytes", -state->frame_size_left);
			return VOD_UNEXPECTED;
		}

		while (state->frame_size_left > 0)
		{
			cur_size = vod_min(state->frame_size_left, (int32_t)sizeof(zero_padding));
			state->frame_size_left -= cur_size;

			rc = state->next_filter->write(state->next_filter_context, zero_padding, cur_size);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}

	return state->next_filter->flush_frame(state->next_filter_context, last_stream_frame);
}


static void
mp4_to_annexb_simulated_start_frame(void* context, output_frame_t* frame)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;
	uint32_t size;

	frame->header_size += state->aud_nal_packet_size;

	state->next_filter->simulated_start_frame(state->next_filter_context, frame);

	size = state->aud_nal_packet_size;
	if (frame->key)
	{
		size += state->extra_data_size;
	}
	state->next_filter->simulated_write(state->next_filter_context, size);
}

static void
mp4_to_annexb_simulated_write(void* context, uint32_t size)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;

	state->next_filter->simulated_write(state->next_filter_context, size);
}

static void
mp4_to_annexb_simulated_flush_frame(void* context, bool_t last_stream_frame)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;

	state->next_filter->simulated_flush_frame(state->next_filter_context, last_stream_frame);
}


const media_filter_t mp4_to_annexb = {
	mp4_to_annexb_start_frame,
	mp4_to_annexb_write,
	mp4_to_annexb_flush_frame,
	mp4_to_annexb_simulated_start_frame,
	mp4_to_annexb_simulated_write,
	mp4_to_annexb_simulated_flush_frame,
};
