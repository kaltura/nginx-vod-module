#include "mp4_cenc_encrypt.h"
#include "mp4_cenc_decrypt.h"
#include "mp4_write_stream.h"
#include "../read_stream.h"
#include "../avc_defs.h"
#include "../udrm.h"

#define MAX_FRAME_RATE (60)

// Note: SNPF = Single Nalu Per Frame

// fragment writer state
enum {
	// regular states
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_ENCRYPT_DATA,
	STATE_COPY_DATA,

	// snpf states
	STATE_CLEAR_BYTES = STATE_PACKET_SIZE,
	STATE_ENCRYPTED_BYTES,
};

// fragment types
typedef struct {
	u_char iv[MP4_AES_CTR_IV_SIZE];
	u_char subsample_count[2];
} cenc_sample_auxiliary_data_t;

u_char*
mp4_cenc_encrypt_write_guid(u_char* p, u_char* guid)
{
	p = vod_sprintf(p, "%02xd%02xd%02xd%02xd-%02xd%02xd-%02xd%02xd-%02xd%02xd-%02xd%02xd%02xd%02xd%02xd%02xd",
		guid[0], guid[1], guid[2], guid[3],
		guid[4], guid[5],
		guid[6], guid[7],
		guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
	return p;
}

static void
mp4_cenc_encrypt_init_track(mp4_cenc_encrypt_state_t* state, media_track_t* track)
{
	// frame state
	state->cur_frame_part = &track->frames;
	state->cur_frame = track->frames.first_frame;
	state->last_frame = track->frames.last_frame;
	state->frame_size_left = 0;
}

static vod_status_t
mp4_cenc_encrypt_init_state(
	mp4_cenc_encrypt_state_t* state,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv)
{
	media_sequence_t* sequence = &media_set->sequences[0];
	drm_info_t* drm_info = (drm_info_t*)sequence->drm_info;
	vod_status_t rc;
	uint64_t iv_int;
	u_char* p;

	// fixed fields
	state->request_context = request_context;
	state->media_set = media_set;
	state->sequence = sequence;
	state->segment_index = segment_index;
	state->segment_writer = *segment_writer;

	// init the aes ctr
	rc = mp4_aes_ctr_init(&state->cipher, request_context, drm_info->key);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// init the output buffer
	write_buffer_init(
		&state->write_buffer,
		request_context,
		segment_writer->write_tail,
		segment_writer->context,
		FALSE);

	// increment the iv by the index of the first frame
	iv_int = parse_be64(iv);
	iv_int += sequence->filtered_clips[0].first_track->first_frame_index;
	// Note: we don't know how many frames were in previous clips (were not parsed), assuming there won't be more than 60 fps
	iv_int += (sequence->filtered_clips[0].first_track->clip_start_time * MAX_FRAME_RATE) / 1000;
	p = state->iv;
	write_be64(p, iv_int);

	// init the first clip
	state->cur_clip = sequence->filtered_clips;
	mp4_cenc_encrypt_init_track(state, state->cur_clip->first_track);

	// saiz / saio
	state->saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	state->saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);

	return VOD_OK;
}

static bool_t
mp4_cenc_encrypt_move_to_next_frame(mp4_cenc_encrypt_state_t* state, bool_t* init_track)
{
	while (state->cur_frame >= state->last_frame)
	{
		if (state->cur_frame_part->next != NULL)
		{
			state->cur_frame_part = state->cur_frame_part->next;
			state->cur_frame = state->cur_frame_part->first_frame;
			state->last_frame = state->cur_frame_part->last_frame;
			break;
		}

		state->cur_clip++;
		if (state->cur_clip >= state->sequence->filtered_clips_end)
		{
			return FALSE;
		}

		mp4_cenc_encrypt_init_track(state, state->cur_clip->first_track);

		if (init_track != NULL)
		{
			*init_track = TRUE;
		}
	}

	return TRUE;
}

static vod_status_t
mp4_cenc_encrypt_start_frame(mp4_cenc_encrypt_state_t* state)
{
	// make sure we have a frame
	if (state->cur_frame >= state->last_frame)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
		"mp4_cenc_encrypt_start_frame: no more frames");
		return VOD_BAD_DATA;
	}

	// get the frame size
	state->frame_size_left = state->cur_frame->size;
	state->cur_frame++;

	// set and increment the iv
	mp4_aes_ctr_set_iv(&state->cipher, state->iv);
	mp4_aes_ctr_increment_be64(state->iv);

	return VOD_OK;
}

////// video fragment functions

static vod_status_t
mp4_cenc_encrypt_video_init_track(mp4_cenc_encrypt_video_state_t* state, media_track_t* track)
{
	switch (track->media_info.codec_id)
	{
	case VOD_CODEC_ID_AVC:
	case VOD_CODEC_ID_HEVC:
		break;

	default:
		vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
			"mp4_cenc_encrypt_video_init_track: codec id %uD is not supported",
			track->media_info.codec_id);
		return VOD_BAD_REQUEST;
	}

	state->nal_packet_size_length = track->media_info.u.video.nal_packet_size_length;

	if (state->nal_packet_size_length < 1 || state->nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
			"mp4_cenc_encrypt_video_init_track: invalid nal packet size length %uD", state->nal_packet_size_length);
		return VOD_BAD_DATA;
	}

	state->codec_id = track->media_info.codec_id;
	state->cur_state = STATE_PACKET_SIZE;
	state->length_bytes_left = state->nal_packet_size_length;
	state->packet_size_left = 0;

	return VOD_OK;
}

static vod_status_t
mp4_cenc_encrypt_video_snpf_build_auxiliary_data(mp4_cenc_encrypt_video_state_t* state)
{
	u_char iv[MP4_AES_CTR_IV_SIZE];
	uint32_t bytes_of_encrypted_data;
	uint16_t bytes_of_clear_data;
	bool_t init_track;
	u_char* p;

	state->default_auxiliary_sample_size = sizeof(cenc_sample_auxiliary_data_t) + sizeof(cenc_sample_auxiliary_data_subsample_t);
	state->saiz_sample_count = state->base.sequence->total_frame_count;

	p = vod_alloc(
		state->base.request_context->pool, 
		state->default_auxiliary_sample_size * state->base.sequence->total_frame_count);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_cenc_encrypt_video_snpf_build_auxiliary_data: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->auxiliary_data.start = p;

	bytes_of_clear_data = state->base.cur_clip->first_track->media_info.u.video.nal_packet_size_length + 1;
	vod_memcpy(iv, state->base.iv, sizeof(iv));

	for (;;)
	{
		init_track = FALSE;
		if (!mp4_cenc_encrypt_move_to_next_frame(&state->base, &init_track))
		{
			break;
		}

		if (init_track)
		{
			bytes_of_clear_data = state->base.cur_clip->first_track->media_info.u.video.nal_packet_size_length + 1;
		}

		// cenc_sample_auxiliary_data_t
		p = vod_copy(p, iv, sizeof(iv));
		mp4_aes_ctr_increment_be64(iv);
		if (state->base.cur_frame->size > 0)
		{
			write_be16(p, 1);		// subsample count

			// cenc_sample_auxiliary_data_subsample_t
			bytes_of_encrypted_data = state->base.cur_frame->size - bytes_of_clear_data;
			write_be16(p, bytes_of_clear_data);
			write_be32(p, bytes_of_encrypted_data);
		}
		else
		{
			write_be16(p, 0);		// subsample count
		}
		state->base.cur_frame++;
	}

	state->auxiliary_data.pos = p;

	// reset the state
	state->base.cur_clip = state->base.sequence->filtered_clips;
	mp4_cenc_encrypt_init_track(&state->base, state->base.cur_clip->first_track);

	return VOD_OK;
}

static vod_status_t
mp4_cenc_encrypt_video_snpf_write_buffer(void* context, u_char* buffer, uint32_t size)
{
	mp4_cenc_encrypt_video_state_t* state = (mp4_cenc_encrypt_video_state_t*)context;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	u_char* cur_end_pos;
	u_char* output;
	uint32_t write_size;
	bool_t init_track;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		switch (state->cur_state)
		{
		case STATE_CLEAR_BYTES:
			// start a new frame if needed
			if (state->base.frame_size_left <= 0)
			{
				rc = mp4_cenc_encrypt_start_frame(&state->base);
				if (rc != VOD_OK)
				{
					return rc;
				}

				if (state->base.frame_size_left <= state->nal_packet_size_length)
				{
					if (state->base.frame_size_left <= 0)
					{
						state->cur_state = STATE_ENCRYPTED_BYTES;
						break;
					}

					vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
						"mp4_cenc_encrypt_video_snpf_write_buffer: frame size %uD too small, nalu size %uD",
						state->base.frame_size_left, state->nal_packet_size_length);
					return VOD_BAD_DATA;
				}
			}

			// copy the clear bytes
			write_size = (uint32_t)(buffer_end - cur_pos);
			write_size = vod_min(write_size, state->length_bytes_left + 1);

			rc = write_buffer_get_bytes(&state->base.write_buffer, write_size, NULL, &output);
			if (rc != VOD_OK)
			{
				return rc;
			}

			// copy the nalu length
			if (write_size >= state->length_bytes_left)
			{
				cur_end_pos = cur_pos + state->length_bytes_left;
				state->length_bytes_left = 0;
			}
			else
			{
				cur_end_pos = cur_pos + write_size;
				state->length_bytes_left -= write_size;
			}

			while (cur_pos < cur_end_pos)
			{
				state->packet_size_left = (state->packet_size_left << 8) | *cur_pos;
				*output++ = *cur_pos++;
			}

			if (cur_pos >= buffer_end)
			{
				break;
			}

			// copy the nalu type
			*output++ = *cur_pos++;

			state->base.frame_size_left -= state->nal_packet_size_length;

			if (state->packet_size_left != state->base.frame_size_left && 
				!state->single_nalu_warning_printed)
			{
				vod_log_error(VOD_LOG_WARN, state->base.request_context->log, 0,
					"mp4_cenc_encrypt_video_snpf_write_buffer: frame does not contain a single nalu, "
					"consider changing vod_min_single_nalu_per_frame_segment, "
					"packet size=%uD, frame size=%uD",
					state->packet_size_left, state->base.frame_size_left);
				state->single_nalu_warning_printed = TRUE;
			}

			state->base.frame_size_left--;

			state->cur_state++;
			// fall through

		case STATE_ENCRYPTED_BYTES:
			write_size = (uint32_t)(buffer_end - cur_pos);
			write_size = vod_min(write_size, state->base.frame_size_left);

			rc = mp4_aes_ctr_write_encrypted(&state->base.cipher, &state->base.write_buffer, cur_pos, write_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_pos += write_size;

			state->base.frame_size_left -= write_size;
			if (state->base.frame_size_left > 0)
			{
				break;
			}

			// finished a packet
			state->cur_state = STATE_CLEAR_BYTES;
			state->length_bytes_left = state->nal_packet_size_length;
			state->packet_size_left = 0;

			// move to the next frame
			init_track = FALSE;
			if (!mp4_cenc_encrypt_move_to_next_frame(&state->base, &init_track))
			{
				return write_buffer_flush(&state->base.write_buffer, FALSE);
			}

			if (init_track)
			{
				rc = mp4_cenc_encrypt_video_init_track(state, state->base.cur_clip->first_track);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

			break;
		}
	}

	return VOD_OK;
}

static vod_status_t
mp4_cenc_encrypt_video_start_frame(mp4_cenc_encrypt_video_state_t* state)
{
	vod_status_t rc;

	// add an auxiliary data entry
	rc = vod_dynamic_buf_reserve(&state->auxiliary_data, sizeof(cenc_sample_auxiliary_data_t));
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_cenc_encrypt_video_start_frame: vod_dynamic_buf_reserve failed %i", rc);
		return rc;
	}

	state->auxiliary_data.pos = vod_copy(state->auxiliary_data.pos, state->base.iv, sizeof(state->base.iv));
	state->auxiliary_data.pos += sizeof(uint16_t);		// write the subsample count on frame end
	state->subsample_count = 0;

	// call the base start frame
	rc = mp4_cenc_encrypt_start_frame(&state->base);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_cenc_encrypt_video_start_frame: mp4_cenc_encrypt_start_frame failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
mp4_cenc_encrypt_video_add_subsample(mp4_cenc_encrypt_video_state_t* state, uint16_t bytes_of_clear_data, uint32_t bytes_of_encrypted_data)
{
	vod_status_t rc;

	rc = vod_dynamic_buf_reserve(&state->auxiliary_data, sizeof(cenc_sample_auxiliary_data_subsample_t));
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_cenc_encrypt_video_add_subsample: vod_dynamic_buf_reserve failed %i", rc);
		return rc;
	}
	write_be16(state->auxiliary_data.pos, bytes_of_clear_data);
	write_be32(state->auxiliary_data.pos, bytes_of_encrypted_data);
	state->subsample_count++;

	return VOD_OK;
}

static vod_status_t
mp4_cenc_encrypt_video_end_frame(mp4_cenc_encrypt_video_state_t* state)
{
	size_t sample_size;
	u_char* p;

	// add the sample size to saiz
	sample_size = sizeof(cenc_sample_auxiliary_data_t)+
		state->subsample_count * sizeof(cenc_sample_auxiliary_data_subsample_t);
	*(state->auxiliary_sample_sizes_pos)++ = sample_size;

	// update subsample count in auxiliary_data
	p = state->auxiliary_data.pos - sample_size + offsetof(cenc_sample_auxiliary_data_t, subsample_count);
	write_be16(p, state->subsample_count);

	return VOD_OK;
}

static u_char
mp4_cenc_encrypt_video_calc_default_auxiliary_sample_size(mp4_cenc_encrypt_video_state_t* state)
{
	u_char default_auxiliary_sample_size;
	u_char* cur_pos;

	if (state->auxiliary_sample_sizes >= state->auxiliary_sample_sizes_pos)
	{
		return 0;
	}

	default_auxiliary_sample_size = *state->auxiliary_sample_sizes;
	for (cur_pos = state->auxiliary_sample_sizes + 1; cur_pos < state->auxiliary_sample_sizes_pos; cur_pos++)
	{
		if (*cur_pos != default_auxiliary_sample_size)
		{
			return 0;
		}
	}

	return default_auxiliary_sample_size;
}

static void
mp4_cenc_encrypt_video_prepare_saiz_saio(mp4_cenc_encrypt_video_state_t* state)
{
	state->default_auxiliary_sample_size = mp4_cenc_encrypt_video_calc_default_auxiliary_sample_size(state);
	state->saiz_sample_count = state->auxiliary_sample_sizes_pos - state->auxiliary_sample_sizes;
	if (state->default_auxiliary_sample_size == 0)
	{
		state->base.saiz_atom_size += state->saiz_sample_count;
	}
}

u_char*
mp4_cenc_encrypt_video_write_saiz_saio(mp4_cenc_encrypt_video_state_t* state, u_char* p, size_t auxiliary_data_offset)
{
	// moof.traf.saiz
	write_atom_header(p, state->base.saiz_atom_size, 's', 'a', 'i', 'z');
	write_be32(p, 0);			// version, flags
	*p++ = state->default_auxiliary_sample_size;
	write_be32(p, state->saiz_sample_count);
	if (state->default_auxiliary_sample_size == 0)
	{
		p = vod_copy(p, state->auxiliary_sample_sizes, state->saiz_sample_count);
	}

	// moof.traf.saio
	write_atom_header(p, state->base.saio_atom_size, 's', 'a', 'i', 'o');
	write_be32(p, 0);			// version, flags
	write_be32(p, 1);			// entry count
	write_be32(p, auxiliary_data_offset);

	return p;
}

static vod_status_t
mp4_cenc_encrypt_video_write_buffer(void* context, u_char* buffer, uint32_t size)
{
	mp4_cenc_encrypt_video_state_t* state = (mp4_cenc_encrypt_video_state_t*)context;
	vod_str_t fragment_header;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	u_char* output;
	u_char nal_type;
	uint32_t write_size;
	int32_t cur_shift;
	size_t ignore;
	bool_t init_track;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		switch (state->cur_state)
		{
		case STATE_PACKET_SIZE:
			if (state->base.frame_size_left <= 0)
			{
				rc = mp4_cenc_encrypt_video_start_frame(state);
				if (rc != VOD_OK)
				{
					return rc;
				}

				if (state->base.frame_size_left <= state->nal_packet_size_length)
				{
					if (state->base.frame_size_left <= 0)
					{
						state->cur_state = STATE_ENCRYPT_DATA;
						break;
					}

					vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
						"mp4_cenc_encrypt_video_write_buffer: frame size %uD too small, nalu size %uD",
						state->base.frame_size_left, state->nal_packet_size_length);
					return VOD_BAD_DATA;
				}
			}

			for (; state->length_bytes_left && cur_pos < buffer_end; state->length_bytes_left--)
			{
				state->packet_size_left = (state->packet_size_left << 8) | *cur_pos++;
			}

			if (cur_pos >= buffer_end)
			{
				break;
			}

			if (state->packet_size_left <= 0)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"mp4_cenc_encrypt_video_write_buffer: zero size packet");
				return VOD_BAD_DATA;
			}

			if (state->packet_size_left > state->base.frame_size_left - state->nal_packet_size_length)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"mp4_cenc_encrypt_video_write_buffer: packet size %uD too big, nalu size %uD, frame size %uD",
					state->packet_size_left, state->nal_packet_size_length, state->base.frame_size_left);
				return VOD_BAD_DATA;
			}

			state->base.frame_size_left -= state->nal_packet_size_length + state->packet_size_left;

			if (state->base.frame_size_left > 0 &&
				state->base.frame_size_left <= state->nal_packet_size_length)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"mp4_cenc_encrypt_video_write_buffer: frame size left %uD too small, nalu size %uD",
					state->base.frame_size_left, state->nal_packet_size_length);
				return VOD_BAD_DATA;
			}

			state->cur_state++;
			// fall through

		case STATE_NAL_TYPE:

			// write the packet size and nal type
			rc = write_buffer_get_bytes(&state->base.write_buffer, state->nal_packet_size_length + 1, NULL, &output);
			if (rc != VOD_OK)
			{
				return rc;
			}

			for (cur_shift = (state->nal_packet_size_length - 1) * 8; cur_shift >= 0; cur_shift -= 8)
			{
				*output++ = (state->packet_size_left >> cur_shift) & 0xff;
			}

			nal_type = *cur_pos++;
			*output++ = nal_type;

			// update the packet size
			if (state->packet_size_left <= 0)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"mp4_cenc_encrypt_video_write_buffer: zero size packet");
				return VOD_BAD_DATA;
			}
			state->packet_size_left--;

			// decide whether to encrypt the nal unit
			state->cur_state = STATE_ENCRYPT_DATA;
			switch (state->codec_id)
			{
			case VOD_CODEC_ID_AVC:
				nal_type &= 0x1f;
				if (nal_type < AVC_NAL_SLICE || nal_type > AVC_NAL_IDR_SLICE)
				{
					state->cur_state = STATE_COPY_DATA;
				}
				break;

			case VOD_CODEC_ID_HEVC:
				nal_type = (nal_type >> 1) & 0x3f;
				if (nal_type >= HEVC_NAL_VPS_NUT)
				{
					state->cur_state = STATE_COPY_DATA;
				}
				break;
			}

			// add the subsample
			if (state->cur_state == STATE_ENCRYPT_DATA)
			{
				rc = mp4_cenc_encrypt_video_add_subsample(state, state->nal_packet_size_length + 1, state->packet_size_left);
			}
			else
			{

				rc = mp4_cenc_encrypt_video_add_subsample(state, state->nal_packet_size_length + 1 + state->packet_size_left, 0);
			}

			if (rc != VOD_OK)
			{
				return rc;
			}

			// fall through

		case STATE_COPY_DATA:
		case STATE_ENCRYPT_DATA:
			write_size = (uint32_t)(buffer_end - cur_pos);
			write_size = vod_min(write_size, state->packet_size_left);
			
			if (state->cur_state == STATE_ENCRYPT_DATA)
			{
				rc = mp4_aes_ctr_write_encrypted(&state->base.cipher, &state->base.write_buffer, cur_pos, write_size);
			}
			else
			{
				rc = write_buffer_write(&state->base.write_buffer, cur_pos, write_size);
			}

			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_pos += write_size;
			state->packet_size_left -= write_size;
			if (state->packet_size_left > 0)
			{
				break;
			}

			// finished a packet
			state->cur_state = STATE_PACKET_SIZE;
			state->length_bytes_left = state->nal_packet_size_length;
			state->packet_size_left = 0;

			if (state->base.frame_size_left > 0)
			{
				break;
			}

			// finished a frame
			rc = mp4_cenc_encrypt_video_end_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			// move to the next frame
			init_track = FALSE;
			if (mp4_cenc_encrypt_move_to_next_frame(&state->base, &init_track))
			{
				if (init_track)
				{
					rc = mp4_cenc_encrypt_video_init_track(state, state->base.cur_clip->first_track);
					if (rc != VOD_OK)
					{
						return rc;
					}
				}

				break;
			}

			// finished all frames
			rc = write_buffer_flush(&state->base.write_buffer, FALSE);
			if (rc != VOD_OK)
			{
				return rc;
			}

			mp4_cenc_encrypt_video_prepare_saiz_saio(state);

			rc = state->build_fragment_header(state, &fragment_header, &ignore);
			if (rc != VOD_OK)
			{
				return rc;
			}

			rc = state->base.segment_writer.write_head(
				state->base.segment_writer.context,
				fragment_header.data,
				fragment_header.len);
			if (rc != VOD_OK)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
					"mp4_cenc_encrypt_video_write_buffer: write_head failed %i", rc);
				return rc;
			}

			break;
		}
	}

	return VOD_OK;
}

vod_status_t
mp4_cenc_encrypt_video_get_fragment_writer(
	segment_writer_t* segment_writer,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	bool_t single_nalu_per_frame,
	mp4_cenc_encrypt_video_build_fragment_header_t build_fragment_header,
	const u_char* iv, 
	vod_str_t* fragment_header, 
	size_t* total_fragment_size)
{
	media_sequence_t* sequence = &media_set->sequences[0];
	mp4_cenc_encrypt_video_state_t* state;
	vod_status_t rc;
	uint32_t initial_size;

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_cenc_encrypt_video_get_fragment_writer: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	rc = mp4_cenc_encrypt_init_state(&state->base, request_context, media_set, segment_index, segment_writer, iv);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_cenc_encrypt_video_get_fragment_writer: mp4_cenc_encrypt_init_state failed %i", rc);
		return rc;
	}

	if (!mp4_cenc_encrypt_move_to_next_frame(&state->base, NULL))
	{
		// an empty segment - write won't be called so we need to write the header here
		state->auxiliary_data.start = NULL;
		state->auxiliary_data.pos = NULL;
		state->auxiliary_sample_sizes = NULL;
		state->auxiliary_sample_sizes_pos = NULL;
		state->default_auxiliary_sample_size = 0;
		state->saiz_sample_count = 0;

		rc = build_fragment_header(state, fragment_header, total_fragment_size);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_cenc_encrypt_video_get_fragment_writer: write_fragment_header failed %i", rc);
			return rc;
		}

		return VOD_OK;
	}
	
	if (single_nalu_per_frame)
	{
		// each frame is a single nal unit, can generate the auxiliary data and write the header now
		state->build_fragment_header = NULL;
		state->single_nalu_warning_printed = FALSE;

		rc = mp4_cenc_encrypt_video_snpf_build_auxiliary_data(state);
		if (rc != VOD_OK)
		{
			return rc;
		}

		rc = build_fragment_header(state, fragment_header, total_fragment_size);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_cenc_encrypt_video_get_fragment_writer: write_fragment_header failed %i", rc);
			return rc;
		}

		segment_writer->write_tail = mp4_cenc_encrypt_video_snpf_write_buffer;
	}
	else
	{
		state->build_fragment_header = build_fragment_header;

		// for progressive AVC a frame usually contains a single nalu, except the first frame which may contain codec copyright info
		initial_size =
			(sizeof(cenc_sample_auxiliary_data_t) + sizeof(cenc_sample_auxiliary_data_subsample_t)) * sequence->total_frame_count +
			sizeof(cenc_sample_auxiliary_data_subsample_t);
		rc = vod_dynamic_buf_init(&state->auxiliary_data, request_context, initial_size);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_cenc_encrypt_video_get_fragment_writer: vod_dynamic_buf_init failed %i", rc);
			return rc;
		}

		state->auxiliary_sample_sizes = vod_alloc(request_context->pool, sequence->total_frame_count);
		if (state->auxiliary_sample_sizes == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_cenc_encrypt_video_get_fragment_writer: vod_alloc failed (2)");
			return VOD_ALLOC_FAILED;
		}
		state->auxiliary_sample_sizes_pos = state->auxiliary_sample_sizes;

		segment_writer->write_tail = mp4_cenc_encrypt_video_write_buffer;
	}

	// init writing for the first track
	rc = mp4_cenc_encrypt_video_init_track(state, state->base.cur_clip->first_track);
	if (rc != VOD_OK)
	{
		return rc;
	}

	segment_writer->write_head = NULL;
	segment_writer->context = state;

	return VOD_OK;
}

////// audio fragment functions

size_t
mp4_cenc_encrypt_audio_get_auxiliary_data_size(mp4_cenc_encrypt_state_t* state)
{
	return MP4_AES_CTR_IV_SIZE * state->sequence->total_frame_count;
}

u_char*
mp4_cenc_encrypt_audio_write_auxiliary_data(mp4_cenc_encrypt_state_t* state, u_char* p)
{
	u_char* end_pos = p + sizeof(state->iv) * state->sequence->total_frame_count;
	u_char iv[MP4_AES_CTR_IV_SIZE];

	vod_memcpy(iv, state->iv, sizeof(iv));

	while (p < end_pos)
	{
		p = vod_copy(p, iv, sizeof(iv));
		mp4_aes_ctr_increment_be64(iv);
	}

	return p;
}

u_char*
mp4_cenc_encrypt_audio_write_saiz_saio(mp4_cenc_encrypt_state_t* state, u_char* p, size_t auxiliary_data_offset)
{
	size_t saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	size_t saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);

	// moof.traf.saiz
	write_atom_header(p, saiz_atom_size, 's', 'a', 'i', 'z');
	write_be32(p, 0);			// version, flags
	*p++ = MP4_AES_CTR_IV_SIZE;				// default auxiliary sample size
	write_be32(p, state->sequence->total_frame_count);

	// moof.traf.saio
	write_atom_header(p, saio_atom_size, 's', 'a', 'i', 'o');
	write_be32(p, 0);			// version, flags
	write_be32(p, 1);			// entry count
	write_be32(p, auxiliary_data_offset);

	return p;
}

static vod_status_t
mp4_cenc_encrypt_audio_write_buffer(void* context, u_char* buffer, uint32_t size)
{
	mp4_cenc_encrypt_state_t* state = (mp4_cenc_encrypt_state_t*)context;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	uint32_t write_size;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		if (state->frame_size_left <= 0)
		{
			rc = mp4_cenc_encrypt_start_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		write_size = (uint32_t)(buffer_end - cur_pos);
		write_size = vod_min(write_size, state->frame_size_left);
		
		rc = mp4_aes_ctr_write_encrypted(&state->cipher, &state->write_buffer, cur_pos, write_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		cur_pos += write_size;
		state->frame_size_left -= write_size;

		if (state->frame_size_left > 0)
		{
			break;
		}

		// finished a frame
		if (!mp4_cenc_encrypt_move_to_next_frame(state, NULL))
		{
			// finished all frames
			rc = write_buffer_flush(&state->write_buffer, FALSE);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}

	return VOD_OK;
}

vod_status_t
mp4_cenc_encrypt_audio_get_fragment_writer(
	segment_writer_t* segment_writer,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	const u_char* iv)
{
	mp4_cenc_encrypt_state_t* state;
	vod_status_t rc;

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_cenc_encrypt_audio_get_fragment_writer: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	rc = mp4_cenc_encrypt_init_state(state, request_context, media_set, segment_index, segment_writer, iv);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_cenc_encrypt_audio_get_fragment_writer: mp4_cenc_encrypt_init_state failed %i", rc);
		return rc;
	}

	segment_writer->write_tail = mp4_cenc_encrypt_audio_write_buffer;
	segment_writer->write_head = NULL;
	segment_writer->context = state;

	if (!mp4_cenc_encrypt_move_to_next_frame(state, NULL))
	{
		return VOD_OK;
	}

	return VOD_OK;
}
