#include "mp4_to_annexb_filter.h"
#include "../read_stream.h"
#include "../avc_defs.h"

#if (VOD_HAVE_OPENSSL_EVP)
#include "sample_aes_avc_filter.h"
#endif // VOD_HAVE_OPENSSL_EVP

// macros
#define THIS_FILTER (MEDIA_FILTER_MP4_TO_ANNEXB)
#define get_context(ctx) ((mp4_to_annexb_state_t*)ctx->context[THIS_FILTER])

// typedefs
typedef struct {
	// input data
	media_filter_t next_filter;

	// fixed
	media_filter_write_t body_write;
	uint8_t unit_type_mask;
	uint8_t aud_unit_type;
	const u_char* aud_nal_packet;
	uint32_t aud_nal_packet_size;
	bool_t sample_aes;

	// data parsed from extra data
	uint32_t nal_packet_size_length;
	const u_char* extra_data;
	uint32_t extra_data_size;

	// state
	int cur_state;
	bool_t first_frame_packet;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;
	int32_t frame_size_left;
} mp4_to_annexb_state_t;

// states
enum {
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_COPY_PACKET,
	STATE_SKIP_PACKET,
};

// constants
static const u_char avc_aud_nal_packet[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };	// f = all pic types + stop bit
static const u_char hevc_aud_nal_packet[] = { 0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50 };	// any slice type
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
mp4_to_annexb_set_media_info(
	media_filter_context_t* context,
	media_info_t* media_info)
{
	mp4_to_annexb_state_t* state = get_context(context);

	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_AVC:
		state->unit_type_mask = 0x1F;
		state->aud_unit_type = AVC_NAL_AUD;
		state->aud_nal_packet = avc_aud_nal_packet;
		state->aud_nal_packet_size = sizeof(avc_aud_nal_packet);
		break;

	case VOD_CODEC_ID_HEVC:
		if (state->sample_aes)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_to_annexb_set_media_info: hevc with sample-aes is not supported");
			return VOD_BAD_REQUEST;
		}

		state->unit_type_mask = (0x3F << 1);
		state->aud_unit_type = (HEVC_NAL_AUD_NUT << 1);
		state->aud_nal_packet = hevc_aud_nal_packet;
		state->aud_nal_packet_size = sizeof(hevc_aud_nal_packet);
		break;

	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_to_annexb_set_media_info: codec id %uD is not supported",
			media_info->codec_id);
		return VOD_BAD_REQUEST;
	}

	state->nal_packet_size_length = media_info->u.video.nal_packet_size_length;
	if (state->nal_packet_size_length < 1 || state->nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_to_annexb_set_media_info: invalid nal packet size length %uD", state->nal_packet_size_length);
		return VOD_BAD_DATA;
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
mp4_to_annexb_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	mp4_to_annexb_state_t* state = get_context(context);
	vod_status_t rc;

	if (frame->size > 0 && frame->size <= state->nal_packet_size_length)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_to_annexb_start_frame: invalid frame size %uD", frame->size);
		return VOD_BAD_DATA;
	}

	state->frame_size_left = frame->size;		// not counting the AUD or extra data since they are written here

	frame->size += state->aud_nal_packet_size;
	frame->header_size += state->aud_nal_packet_size;
	if (frame->key)
	{
		frame->size += state->extra_data_size;
	}

	rc = state->next_filter.start_frame(context, frame);
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
	rc = state->next_filter.write(context, state->aud_nal_packet, state->aud_nal_packet_size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (frame->key)
	{
		rc = state->next_filter.write(context, state->extra_data, state->extra_data_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t 
mp4_to_annexb_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	mp4_to_annexb_state_t* state = get_context(context);
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

			if (state->packet_size_left <= 0)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_to_annexb_write: zero size packet");
				return VOD_BAD_DATA;
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

#if (VOD_HAVE_OPENSSL_EVP)
			if (state->sample_aes)
			{
				rc = sample_aes_avc_start_nal_unit(
					context, 
					unit_type, 
					state->packet_size_left);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
#endif // VOD_HAVE_OPENSSL_EVP
						
			if (state->first_frame_packet)
			{
				state->first_frame_packet = FALSE;
				state->frame_size_left -= sizeof(nal_marker);
				rc = state->next_filter.write(context, nal_marker, sizeof(nal_marker));
			}
			else
			{
				state->frame_size_left -= (sizeof(nal_marker) - 1);
				rc = state->next_filter.write(context, nal_marker + 1, sizeof(nal_marker) - 1);
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
				rc = state->body_write(context, buffer, write_size);
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
mp4_to_annexb_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	mp4_to_annexb_state_t* state = get_context(context);
	vod_status_t rc;
	int32_t cur_size;

	if (state->nal_packet_size_length == 4 && !state->sample_aes)
	{
		if (state->frame_size_left < 0)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_to_annexb_flush_frame: frame exceeded the calculated size by %D bytes", -state->frame_size_left);
			return VOD_UNEXPECTED;
		}

		while (state->frame_size_left > 0)
		{
			cur_size = vod_min(state->frame_size_left, (int32_t)sizeof(zero_padding));
			state->frame_size_left -= cur_size;

			rc = state->next_filter.write(context, zero_padding, cur_size);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}

	return state->next_filter.flush_frame(context, last_stream_frame);
}


static void
mp4_to_annexb_simulated_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	mp4_to_annexb_state_t* state = get_context(context);
	uint32_t size;

	frame->header_size += state->aud_nal_packet_size;

	state->next_filter.simulated_start_frame(context, frame);

	size = state->aud_nal_packet_size;
	if (frame->key)
	{
		size += state->extra_data_size;
	}
	state->next_filter.simulated_write(context, size);
}


vod_status_t
mp4_to_annexb_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	hls_encryption_params_t* encryption_params)
{
	mp4_to_annexb_state_t* state;
	request_context_t* request_context = context->request_context;
#if (VOD_HAVE_OPENSSL_EVP)
	vod_status_t rc;
#endif // VOD_HAVE_OPENSSL_EVP

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_to_annexb_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// init sample aes
#if (VOD_HAVE_OPENSSL_EVP)
	if (encryption_params->type == HLS_ENC_SAMPLE_AES)
	{
		rc = sample_aes_avc_filter_init(
			filter,
			context,
			encryption_params->key,
			encryption_params->iv);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->sample_aes = TRUE;
		state->body_write = sample_aes_avc_filter_write_nal_body;
	}
	else
#endif // VOD_HAVE_OPENSSL_EVP
	{
		state->sample_aes = FALSE;
		state->body_write = filter->write;
	}

	// save required functions
	state->next_filter = *filter;

	// override functions
	filter->start_frame = mp4_to_annexb_start_frame;
	filter->write = mp4_to_annexb_write;
	filter->flush_frame = mp4_to_annexb_flush_frame;
	filter->simulated_start_frame = mp4_to_annexb_simulated_start_frame;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}

