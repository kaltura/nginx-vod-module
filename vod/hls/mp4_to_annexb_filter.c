#include "mp4_to_annexb_filter.h"
#include "../read_stream.h"

// h264 NAL unit types
enum {
    NAL_SLICE           = 1,
    NAL_DPA             = 2,
    NAL_DPB             = 3,
    NAL_DPC             = 4,
    NAL_IDR_SLICE       = 5,
    NAL_SEI             = 6,
    NAL_SPS             = 7,
    NAL_PPS             = 8,
    NAL_AUD             = 9,
    NAL_END_SEQUENCE    = 10,
    NAL_END_STREAM      = 11,
    NAL_FILLER_DATA     = 12,
    NAL_SPS_EXT         = 13,
    NAL_AUXILIARY_SLICE = 19,
    NAL_FF_IGNORE       = 0xff0f001,
};

// states
enum {
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_COPY_PACKET,
	STATE_SKIP_PACKET,
};

// constants
static const u_char aud_nal_packet[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };	// f = all pic types + stop bit
static const u_char nal_marker[] = { 0x00, 0x00, 0x00, 0x01 };

vod_status_t 
mp4_to_annexb_init(
	mp4_to_annexb_state_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	const u_char* extra_data, 
	uint32_t extra_data_size,
	uint32_t nal_packet_size_length)
{
	if (nal_packet_size_length < 1 || nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_to_annexb_init: invalid nal packet size length %uD", nal_packet_size_length);
		return VOD_BAD_DATA;
	}

	state->request_context = request_context;
	state->first_idr = TRUE;
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	state->sps_pps = extra_data;
	state->sps_pps_size = extra_data_size;
	state->nal_packet_size_length = nal_packet_size_length;

	return VOD_OK;
}

bool_t 
mp4_to_annexb_simulation_supported(mp4_to_annexb_state_t* state)
{
	/* When the packet size field length is 4 we can bound the output size - since every 4-byte length
		field is transformed to a \0\0\0\x01 or \0\0\x01 NAL marker, the output size is <= the input size.
		When the packet size field length is less than 4, the output size may be greater than input size by
		the number of NAL packets. Since we don't know this number in advance we have no way to bound the
		output size. Luckily, ffmpeg always uses 4 byte size fields - see ff_isom_write_avcc */
	return state->nal_packet_size_length == 4;
}

static vod_status_t 
mp4_to_annexb_start_frame(void* context, output_frame_t* frame)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;
	vod_status_t rc;

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
	state->key_frame = frame->key;
	state->frame_size_left = frame->original_size;		// not adding the aud packet since we're just about to write it
	if (frame->key)
	{
		state->frame_size_left += state->sps_pps_size;
	}

	// write access unit delimiter packet
	return state->next_filter->write(state->next_filter_context, aud_nal_packet, sizeof(aud_nal_packet));
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
			unit_type = *buffer & 0x1f;
			if (unit_type == NAL_AUD)
			{
				state->cur_state = STATE_SKIP_PACKET;
				break;
			}
			
			switch (unit_type)
			{
			case NAL_SLICE:
				state->first_idr = TRUE;
				break;

			case NAL_IDR_SLICE:
			case NAL_SPS:
			case NAL_PPS:
				if (state->key_frame && state->first_idr)
				{
					state->frame_size_left -= state->sps_pps_size;
					rc = state->next_filter->write(state->next_filter_context, state->sps_pps, state->sps_pps_size);
					if (rc != VOD_OK)
					{
						return rc;
					}
					state->first_idr = FALSE;
				}
				break;
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
				rc = state->next_filter->write(state->next_filter_context, buffer, write_size);
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
mp4_to_annexb_flush_frame(void* context, int32_t margin_size)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;

	if (state->nal_packet_size_length == 4)
	{
		if (state->frame_size_left < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mp4_to_annexb_flush_frame: frame exceeded the calculated size by %D bytes", -state->frame_size_left);
			return VOD_UNEXPECTED;
		}

		margin_size += state->frame_size_left;
	}

	return state->next_filter->flush_frame(state->next_filter_context, margin_size);
}

static void 
mp4_to_annexb_simulated_write(void* context, output_frame_t* frame)
{
	mp4_to_annexb_state_t* state = (mp4_to_annexb_state_t*)context;

	frame->original_size += sizeof(aud_nal_packet);
	if (frame->key)
	{
		frame->original_size += state->sps_pps_size;
	}

	state->next_filter->simulated_write(state->next_filter_context, frame);
}

const media_filter_t mp4_to_annexb = {
	mp4_to_annexb_start_frame,
	mp4_to_annexb_write,
	mp4_to_annexb_flush_frame,
	mp4_to_annexb_simulated_write,
};
