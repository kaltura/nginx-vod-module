#include "mp4_encrypt.h"
#include "mp4_builder.h"
#include "../read_stream.h"

#define MAX_FRAME_RATE (60)

// fragment writer state
enum {
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_PACKET_DATA,
};

// fragment types
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char default_sample_info_size;
	u_char sample_count[4];
} saiz_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char entry_count[4];
	u_char offset[4];
} saio_atom_t;

typedef struct {
	u_char iv[MP4_ENCRYPT_IV_SIZE];
	u_char subsample_count[2];
} cenc_sample_auxiliary_data_t;

typedef struct {
	u_char bytes_of_clear_data[2];
	u_char bytes_of_encrypted_data[4];
} cenc_sample_auxiliary_data_subsample_t;

u_char*
mp4_encrypt_write_guid(u_char* p, u_char* guid)
{
	p = vod_sprintf(p, "%02xd%02xd%02xd%02xd-%02xd%02xd-%02xd%02xd-%02xd%02xd-%02xd%02xd%02xd%02xd%02xd%02xd",
		guid[0], guid[1], guid[2], guid[3],
		guid[4], guid[5],
		guid[6], guid[7],
		guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
	return p;
}

static void
mp4_encrypt_increment_be64(u_char* counter)
{
	u_char* cur_pos;

	for (cur_pos = counter + 7; cur_pos >= counter; cur_pos--)
	{
		(*cur_pos)++;
		if (*cur_pos != 0)
		{
			break;
		}
	}
}

static vod_status_t
mp4_encrypt_encrypt(mp4_encrypt_state_t* state, u_char* buffer, uint32_t size)
{
	u_char* encrypted_counter_pos;
	u_char* cur_end_pos;
	u_char* buffer_end = buffer + size;
	int out_size;

	while (buffer < buffer_end)
	{
		if (state->block_offset == 0)
		{
			if (1 != EVP_EncryptUpdate(
				&state->cipher, 
				state->encrypted_counter, 
				&out_size, 
				state->counter, 
				sizeof(state->counter)) || 
				out_size != sizeof(state->encrypted_counter))
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_encrypt_encrypt: EVP_EncryptUpdate failed");
				return VOD_UNEXPECTED;
			}

			mp4_encrypt_increment_be64(state->counter + 8);
		}

		encrypted_counter_pos = state->encrypted_counter + state->block_offset;
		cur_end_pos = buffer + MP4_ENCRYPT_COUNTER_SIZE - state->block_offset;
		cur_end_pos = vod_min(cur_end_pos, buffer_end);

		state->block_offset += cur_end_pos - buffer;
		state->block_offset &= (MP4_ENCRYPT_COUNTER_SIZE - 1);

		while (buffer < cur_end_pos)
		{
			*buffer ^= *encrypted_counter_pos;
			buffer++;
			encrypted_counter_pos++;
		}
	}

	return VOD_OK;
}

static void
mp4_encrypt_cleanup(mp4_encrypt_state_t* state)
{
	EVP_CIPHER_CTX_cleanup(&state->cipher);
}

static void
mp4_encrypt_init_track(mp4_encrypt_state_t* state, media_track_t* track)
{
	// frame state
	state->cur_frame = track->first_frame;
	state->last_frame = track->last_frame;
	state->frame_count = track->frame_count;
	state->frame_size_left = 0;
}

static vod_status_t
mp4_encrypt_init_state(
	mp4_encrypt_state_t* state,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv)
{
	media_sequence_t* sequence = &media_set->sequences[0];
	mp4_encrypt_info_t* drm_info = (mp4_encrypt_info_t*)sequence->drm_info;
	vod_pool_cleanup_t *cln;
	uint64_t iv_int;
	u_char* p;

	// fixed fields
	state->request_context = request_context;
	state->media_set = media_set;
	state->sequence = sequence;
	state->segment_index = segment_index;
	state->segment_writer = *segment_writer;

	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_init_state: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)mp4_encrypt_cleanup;
	cln->data = state;

	EVP_CIPHER_CTX_init(&state->cipher);

	if (1 != EVP_EncryptInit_ex(&state->cipher, EVP_aes_128_ecb(), NULL, drm_info->key, NULL))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_encrypt_init_state: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	// increment the iv by the index of the first frame
	iv_int = parse_be64(iv);
	iv_int += sequence->filtered_clips[0].first_track->first_frame_index;
	// Note: we don't know how many frames were in previous clips (were not parsed), assuming there won't be more than 60 fps
	iv_int += (sequence->filtered_clips[0].first_track->clip_sequence_offset * MAX_FRAME_RATE) / sequence->filtered_clips[0].first_track->media_info.timescale;
	p = state->iv;
	write_qword(p, iv_int);

	// init the first clip
	state->cur_clip = sequence->filtered_clips;
	mp4_encrypt_init_track(state, state->cur_clip->first_track);

	// saiz / saio
	state->saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	state->saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);

	return VOD_OK;
}

static bool_t
mp4_encrypt_move_to_next_frame(mp4_encrypt_state_t* state, bool_t* init_track)
{
	*init_track = FALSE;
	while (state->cur_frame >= state->last_frame)
	{
		state->cur_clip++;
		if (state->cur_clip >= state->sequence->filtered_clips_end)
		{
			return FALSE;
		}

		mp4_encrypt_init_track(state, state->cur_clip->first_track);

		*init_track = TRUE;
	}

	return TRUE;
}

static vod_status_t
mp4_encrypt_start_frame(mp4_encrypt_state_t* state, bool_t* init_track)
{
	// make sure we have a frame
	if (!mp4_encrypt_move_to_next_frame(state, init_track))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
		"mp4_encrypt_start_frame: no more frames");
		return VOD_BAD_DATA;
	}

	// get the frame size
	state->frame_size_left = state->cur_frame->size;
	state->cur_frame++;

	// initialize the counter and block offset
	vod_memcpy(state->counter, state->iv, sizeof(state->iv));
	vod_memzero(state->counter + sizeof(state->iv), sizeof(state->counter) - sizeof(state->iv));
	state->block_offset = 0;

	// increment the iv
	mp4_encrypt_increment_be64(state->iv);

	return VOD_OK;
}

////// video fragment functions

static vod_status_t
mp4_encrypt_video_init_track(mp4_encrypt_video_state_t* state, media_track_t* track)
{
	state->nal_packet_size_length = track->media_info.u.video.nal_packet_size_length;

	if (state->nal_packet_size_length < 1 || state->nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
			"mp4_encrypt_video_init_track: invalid nal packet size length %uD", state->nal_packet_size_length);
		return VOD_BAD_DATA;
	}

	state->cur_state = STATE_PACKET_SIZE;
	state->length_bytes_left = state->nal_packet_size_length;
	state->packet_size_left = 0;

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_video_start_frame(mp4_encrypt_video_state_t* state)
{
	vod_status_t rc;
	bool_t init_track;

	// add an auxiliary data entry
	rc = vod_dynamic_buf_reserve(&state->auxiliary_data, sizeof(cenc_sample_auxiliary_data_t));
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_encrypt_video_start_frame: vod_dynamic_buf_reserve failed %i", rc);
		return rc;
	}

	state->auxiliary_data.pos = vod_copy(state->auxiliary_data.pos, state->base.iv, sizeof(state->base.iv));
	state->auxiliary_data.pos += sizeof(uint16_t);		// write the subsample count on frame end
	state->subsample_count = 0;

	// call the base start frame
	rc = mp4_encrypt_start_frame(&state->base, &init_track);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_encrypt_video_start_frame: mp4_encrypt_start_frame failed %i", rc);
		return rc;
	}

	if (init_track)
	{
		rc = mp4_encrypt_video_init_track(state, state->base.cur_clip->first_track);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_video_add_subsample(mp4_encrypt_video_state_t* state, uint16_t bytes_of_clear_data, uint32_t bytes_of_encrypted_data)
{
	vod_status_t rc;

	rc = vod_dynamic_buf_reserve(&state->auxiliary_data, sizeof(cenc_sample_auxiliary_data_subsample_t));
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mp4_encrypt_video_add_subsample: vod_dynamic_buf_reserve failed %i", rc);
		return rc;
	}
	write_word(state->auxiliary_data.pos, bytes_of_clear_data);
	write_dword(state->auxiliary_data.pos, bytes_of_encrypted_data);
	state->subsample_count++;

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_video_end_frame(mp4_encrypt_video_state_t* state)
{
	size_t sample_size;
	u_char* p;

	// add the sample size to saiz
	sample_size = sizeof(cenc_sample_auxiliary_data_t)+
		state->subsample_count * sizeof(cenc_sample_auxiliary_data_subsample_t);
	*(state->auxiliary_sample_sizes_pos)++ = sample_size;

	// update subsample count in auxiliary_data
	p = state->auxiliary_data.pos - sample_size + offsetof(cenc_sample_auxiliary_data_t, subsample_count);
	write_word(p, state->subsample_count);

	return VOD_OK;
}

static u_char
mp4_encrypt_video_calc_default_auxiliary_sample_size(mp4_encrypt_video_state_t* state)
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
mp4_encrypt_video_prepare_saiz_saio(mp4_encrypt_video_state_t* state)
{
	state->default_auxiliary_sample_size = mp4_encrypt_video_calc_default_auxiliary_sample_size(state);
	state->saiz_sample_count = state->auxiliary_sample_sizes_pos - state->auxiliary_sample_sizes;
	if (state->default_auxiliary_sample_size == 0)
	{
		state->base.saiz_atom_size += state->saiz_sample_count;
	}
}

u_char*
mp4_encrypt_video_write_saiz_saio(mp4_encrypt_video_state_t* state, u_char* p, size_t auxiliary_data_offset)
{
	// moof.traf.saiz
	write_atom_header(p, state->base.saiz_atom_size, 's', 'a', 'i', 'z');
	write_dword(p, 0);			// version, flags
	*p++ = state->default_auxiliary_sample_size;
	write_dword(p, state->saiz_sample_count);
	if (state->default_auxiliary_sample_size == 0)
	{
		p = vod_copy(p, state->auxiliary_sample_sizes, state->saiz_sample_count);
	}

	// moof.traf.saio
	write_atom_header(p, state->base.saio_atom_size, 's', 'a', 'i', 'o');
	write_dword(p, 0);			// version, flags
	write_dword(p, 1);			// entry count
	write_dword(p, auxiliary_data_offset);

	return p;
}

static vod_status_t
mp4_encrypt_video_write_buffer(void* context, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	mp4_encrypt_video_state_t* state = (mp4_encrypt_video_state_t*)context;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	uint32_t write_size;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		switch (state->cur_state)
		{
		case STATE_PACKET_SIZE:
			if (state->base.frame_size_left <= 0)
			{
				for (;;)
				{
					rc = mp4_encrypt_video_start_frame(state);
					if (rc != VOD_OK)
					{
						return rc;
					}

					if (state->base.frame_size_left > 0)
					{
						break;
					}

					rc = mp4_encrypt_video_end_frame(state);
					if (rc != VOD_OK)
					{
						return rc;
					}
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

			if (state->base.frame_size_left < state->nal_packet_size_length + state->packet_size_left)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"mp4_encrypt_video_write_buffer: frame size %uD too small, nalu size %uD packet size %uD",
					state->base.frame_size_left, state->nal_packet_size_length, state->packet_size_left);
				return VOD_BAD_DATA;
			}

			state->base.frame_size_left -= state->nal_packet_size_length + state->packet_size_left;

			state->cur_state++;
			// fall through

		case STATE_NAL_TYPE:
			cur_pos++;
			if (state->packet_size_left <= 0)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"mp4_encrypt_video_write_buffer: zero size packet");
				return VOD_BAD_DATA;
			}
			state->packet_size_left--;

			rc = mp4_encrypt_video_add_subsample(state, state->nal_packet_size_length + 1, state->packet_size_left);
			if (rc != VOD_OK)
			{
				return rc;
			}
			state->cur_state++;
			// fall through

		case STATE_PACKET_DATA:
			write_size = vod_min(state->packet_size_left, (uint32_t)(buffer_end - cur_pos));

			rc = mp4_encrypt_encrypt(&state->base, cur_pos, write_size);
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
			rc = mp4_encrypt_video_end_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (state->base.cur_frame < state->base.last_frame)
			{
				break;
			}

			// finished all frames
			mp4_encrypt_video_prepare_saiz_saio(state);

			rc = state->write_fragment_header(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
			break;
		}
	}

	return state->base.segment_writer.write_tail(state->base.segment_writer.context, buffer, size, reuse_buffer);
}

vod_status_t
mp4_encrypt_video_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	mp4_encrypt_video_write_fragment_header_t write_fragment_header,
	segment_writer_t* segment_writer,
	const u_char* iv)
{
	media_sequence_t* sequence = &media_set->sequences[0];
	mp4_encrypt_video_state_t* state;
	vod_status_t rc;
	uint32_t initial_size;
	bool_t ignore;

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_video_get_fragment_writer: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	rc = mp4_encrypt_init_state(&state->base, request_context, media_set, segment_index, segment_writer, iv);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_video_get_fragment_writer: mp4_encrypt_init_state failed %i", rc);
		return rc;
	}

	// fixed members
	state->write_fragment_header = write_fragment_header;

	// for progressive AVC a frame usually contains a single nalu, except the first frame which may contain codec copyright info
	initial_size = 
		(sizeof(cenc_sample_auxiliary_data_t) + sizeof(cenc_sample_auxiliary_data_subsample_t)) * sequence->total_frame_count + 
		sizeof(cenc_sample_auxiliary_data_subsample_t);
	rc = vod_dynamic_buf_init(&state->auxiliary_data, request_context, initial_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_video_get_fragment_writer: vod_dynamic_buf_init failed %i", rc);
		return rc;
	}

	state->auxiliary_sample_sizes = vod_alloc(request_context->pool, sequence->total_frame_count);
	if (state->auxiliary_sample_sizes == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_video_get_fragment_writer: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	state->auxiliary_sample_sizes_pos = state->auxiliary_sample_sizes;

	if (!mp4_encrypt_move_to_next_frame(&state->base, &ignore))
	{
		// an empty segment - write won't be called so we need to write the header here
		mp4_encrypt_video_prepare_saiz_saio(state);

		rc = state->write_fragment_header(state);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_encrypt_video_get_fragment_writer: write_fragment_header failed %i", rc);
			return rc;
		}

		return VOD_OK;
	}

	// init writing for the first track
	rc = mp4_encrypt_video_init_track(state, state->base.cur_clip->first_track);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->write_tail = mp4_encrypt_video_write_buffer;
	result->write_head = NULL;
	result->context = state;

	return VOD_OK;
}

size_t 
mp4_encrypt_audio_get_auxiliary_data_size(mp4_encrypt_state_t* state)
{
	return MP4_ENCRYPT_IV_SIZE * state->sequence->total_frame_count;
}

u_char*
mp4_encrypt_audio_write_auxiliary_data(mp4_encrypt_state_t* state, u_char* p)
{
	u_char* end_pos = p + sizeof(state->iv) * state->frame_count;
	u_char iv[MP4_ENCRYPT_IV_SIZE];

	vod_memcpy(iv, state->iv, sizeof(iv));

	while (p < end_pos)
	{
		p = vod_copy(p, iv, sizeof(iv));
		mp4_encrypt_increment_be64(iv);
	}

	return p;
}

u_char*
mp4_encrypt_audio_write_saiz_saio(mp4_encrypt_state_t* state, u_char* p, size_t auxiliary_data_offset)
{
	size_t saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	size_t saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);

	// moof.traf.saiz
	write_atom_header(p, saiz_atom_size, 's', 'a', 'i', 'z');
	write_dword(p, 0);			// version, flags
	*p++ = MP4_ENCRYPT_IV_SIZE;				// default auxiliary sample size
	write_dword(p, state->frame_count);

	// moof.traf.saio
	write_atom_header(p, saio_atom_size, 's', 'a', 'i', 'o');
	write_dword(p, 0);			// version, flags
	write_dword(p, 1);			// entry count
	write_dword(p, auxiliary_data_offset);

	return p;
}

static vod_status_t
mp4_encrypt_audio_write_buffer(void* context, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	mp4_encrypt_state_t* state = (mp4_encrypt_state_t*)context;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	uint32_t write_size;
	bool_t ignore;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		while (state->frame_size_left <= 0)
		{
			rc = mp4_encrypt_start_frame(state, &ignore);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		write_size = vod_min(state->frame_size_left, (uint32_t)(buffer_end - cur_pos));

		rc = mp4_encrypt_encrypt(state, cur_pos, write_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		cur_pos += write_size;
		state->frame_size_left -= write_size;
	}

	return state->segment_writer.write_tail(state->segment_writer.context, buffer, size, reuse_buffer);
}

vod_status_t
mp4_encrypt_audio_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv)
{
	mp4_encrypt_state_t* state;
	vod_status_t rc;

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_audio_get_fragment_writer: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	rc = mp4_encrypt_init_state(state, request_context, media_set, segment_index, segment_writer, iv);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_audio_get_fragment_writer: mp4_encrypt_init_state failed %i", rc);
		return rc;
	}

	result->write_tail = mp4_encrypt_audio_write_buffer;
	result->write_head = NULL;
	result->context = state;

	return VOD_OK;
}
