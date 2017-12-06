#include "../input/frames_source_cache.h"
#include "hds_fragment.h"
#include "hds_amf0_encoder.h"
#include "../write_buffer.h"
#include "../mp4/mp4_defs.h"
#include "../mp4/mp4_fragment.h"
#include "../aes_defs.h"

// adobe mux packet definitions
#define TAG_TYPE_AUDIO (8)
#define TAG_TYPE_VIDEO (9)
#define TAG_TYPE_ENCRYPTED_AUDIO (0x28)
#define TAG_TYPE_ENCRYPTED_VIDEO (0x29)

#define AVC_PACKET_TYPE_SEQUENCE_HEADER (0)
#define AVC_PACKET_TYPE_NALU 			(1)
#define AVC_PACKET_TYPE_END_OF_SEQUENCE (2)

#define FRAME_TYPE_KEY_FRAME 	(1)
#define FRAME_TYPE_INTER_FRAME 	(2)

#define SOUND_RATE_5_5_KHZ	(0)
#define SOUND_RATE_11_KHZ	(1)
#define SOUND_RATE_22_KHZ	(2)
#define SOUND_RATE_44_KHZ	(3)

#define SOUND_SIZE_8_BIT	(0)
#define SOUND_SIZE_16_BIT	(1)

#define SOUND_TYPE_MONO		(0)
#define SOUND_TYPE_STEREO	(1)

#define AAC_PACKET_TYPE_SEQUENCE_HEADER (0)
#define AAC_PACKET_TYPE_RAW 			(1)

#define TRUN_SIZE_SINGLE_VIDEO_FRAME (ATOM_HEADER_SIZE + sizeof(trun_atom_t) + sizeof(trun_video_frame_t))
#define TRUN_SIZE_SINGLE_AUDIO_FRAME (ATOM_HEADER_SIZE + sizeof(trun_atom_t) + sizeof(trun_audio_frame_t))

#define HDS_AES_KEY_SIZE (16)

// macros
#define write_be24(p, dw)			\
{									\
	*(p)++ = ((dw) >> 16) & 0xFF;	\
	*(p)++ = ((dw) >> 8) & 0xFF;	\
	*(p)++ = (dw)& 0xFF;			\
}

// atom structs
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char extra_flags[1];		// 1 bit long ids, 1 bit long offsets, 1 bit global entries
	u_char timescale[4];
	u_char entries[4];
} afra_atom_t;

typedef struct {
	u_char pts[8];
	u_char offset[8];
} afra_entry_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char track_id[4];
	u_char base_data_offset[8];
	u_char sample_desc_index[4];
} hds_tfhd_atom_t;

// frame tags
typedef struct {
	u_char tag_type[1];		// 2 bits reserved, 1 bit filter, 5 bits tag type
	u_char data_size[3];
	u_char timestamp[3];
	u_char timestamp_ext[1];
	u_char stream_id[3];
} adobe_mux_packet_header_t;

typedef struct {
	u_char frame_type[1];		// 4 bits frame type, 4 bits codec id
	u_char avc_packet_type[1];
	u_char avc_comp_time_offset[3];
} video_tag_header_avc;

typedef struct {
	u_char sound_info[1];		// 4 bits format, 2 bits rate, 1 bit size, 1 bit type
} audio_tag_header;

typedef struct {
	u_char sound_info[1];		// 4 bits format, 2 bits rate, 1 bit size, 1 bit type
	u_char aac_packet_type[1];
} audio_tag_header_aac;

// state
typedef struct {
	media_track_t* track;
	int media_type;
	uint8_t sound_info;
	uint32_t frame_count;
	uint32_t index;
	uint32_t tag_size;

	uint64_t first_frame_time_offset;
	uint64_t next_frame_time_offset;

	// input frames
	frame_list_part_t* first_frame_part;
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	media_clip_source_t* source;

	// frame output offsets
	uint32_t* first_frame_output_offset;
	uint32_t* cur_frame_output_offset;
} hds_muxer_stream_state_t;

struct hds_muxer_state_s {
	// fixed
	request_context_t* request_context;
	uint8_t video_tag_type;
	uint8_t audio_tag_type;

	hds_muxer_stream_state_t* first_stream;
	hds_muxer_stream_state_t* last_stream;
	uint32_t codec_config_size;

	write_buffer_state_t write_buffer_state;

	// cur clip state
	media_set_t* media_set;
	media_track_t* first_clip_track;

	input_frame_t* cur_frame;
	int cache_slot_id;
	frames_source_t* frames_source;
	void* frames_source_context;
	bool_t first_time;

	uint32_t frame_header_size;
	uint32_t frame_size;

	// encryption state
	hds_encryption_type_t enc_type;
#if (VOD_HAVE_OPENSSL_EVP)
	u_char enc_key[HDS_AES_KEY_SIZE];
	u_char enc_iv[AES_BLOCK_SIZE];
	EVP_CIPHER_CTX* cipher;
#endif //(VOD_HAVE_OPENSSL_EVP)
};

typedef struct {
	u_char is_encrypted[1];
	u_char iv[AES_BLOCK_SIZE];
} hds_selective_encryption_filter_params_t;

static vod_status_t hds_muxer_start_frame(hds_muxer_state_t* state);

#if (VOD_HAVE_OPENSSL_EVP)

#define ENCRYPTION_TAG_HEADER_SIZE sizeof(hds_encryption_tag_header)

static u_char hds_encryption_tag_header[] = {
	0x01,					// num filters
	0x53, 0x45, 0x00,		// filter name (SE)
	0x00, 0x00, 0x11,		// params len = sizeof(hds_selective_encryption_filter_params_t)
};

////// encryption functions
static void
hds_muxer_encrypt_cleanup(hds_muxer_state_t* state)
{
	EVP_CIPHER_CTX_free(state->cipher);
}

static vod_status_t
hds_muxer_encrypt_init(
	hds_muxer_state_t* state,
	hds_encryption_params_t* encryption_params)
{
	vod_pool_cleanup_t *cln;

	cln = vod_pool_cleanup_add(state->request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"hds_muxer_encrypt_init: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	state->cipher = EVP_CIPHER_CTX_new();
	if (state->cipher == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"hds_muxer_encrypt_init: EVP_CIPHER_CTX_new failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)hds_muxer_encrypt_cleanup;
	cln->data = state;

	vod_memcpy(state->enc_key, encryption_params->key, sizeof(state->enc_key));
	vod_memcpy(state->enc_iv, encryption_params->iv, sizeof(state->enc_iv));

	state->video_tag_type = TAG_TYPE_ENCRYPTED_VIDEO;
	state->audio_tag_type = TAG_TYPE_ENCRYPTED_AUDIO;

	return VOD_OK;
}

static u_char*
hds_muxer_encrypt_write_header(hds_muxer_state_t* state, u_char* p)
{
	p = vod_copy(p, hds_encryption_tag_header, ENCRYPTION_TAG_HEADER_SIZE);

	// selective encryption filter params
	*p++ = 0x80;		// packet is encrypted
	p = vod_copy(p, state->enc_iv, sizeof(state->enc_iv));

	return p;
}

static vod_status_t
hds_muxer_encrypt_start_frame(hds_muxer_state_t* state)
{
	// reset the IV (it is ok to call EVP_EncryptInit_ex several times without cleanup)
	if (1 != EVP_EncryptInit_ex(state->cipher, EVP_aes_128_cbc(), NULL, state->enc_key, state->enc_iv))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"hds_muxer_encrypt_start_frame: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

static vod_status_t
hds_muxer_encrypt_write(
	hds_muxer_state_t* state, 
	u_char* buffer, 
	uint32_t size, 
	bool_t frame_done)
{
	vod_status_t rc;
	u_char* buffer_end = buffer + size;
	u_char* out_buffer;
	size_t cur_size;
	int out_size;

	while (buffer < buffer_end)
	{
		rc = write_buffer_get_bytes(
			&state->write_buffer_state,
			AES_BLOCK_SIZE,
			&cur_size,
			&out_buffer);
		if (rc != VOD_OK)
		{
			return rc;
		}

		cur_size = aes_round_down_to_block(cur_size);
		if ((size_t)(buffer_end - buffer) < cur_size)
		{
			cur_size = buffer_end - buffer;
		}

		if (1 != EVP_EncryptUpdate(state->cipher, out_buffer, &out_size, buffer, cur_size))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"hds_muxer_encrypt_write: EVP_EncryptUpdate failed");
			return VOD_UNEXPECTED;
		}

		buffer += cur_size;
		state->write_buffer_state.cur_pos += out_size;
	}

	if (frame_done)
	{
		rc = write_buffer_get_bytes(
			&state->write_buffer_state,
			AES_BLOCK_SIZE,
			NULL,
			&out_buffer);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (1 != EVP_EncryptFinal_ex(state->cipher, out_buffer, &out_size))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"hds_muxer_encrypt_write: EVP_EncryptFinal_ex failed");
			return VOD_UNEXPECTED;
		}

		vod_memcpy(state->enc_iv, out_buffer, sizeof(state->enc_iv));
	}

	return VOD_OK;
}
#else

#define ENCRYPTION_TAG_HEADER_SIZE (0)

// empty stubs
static vod_status_t
hds_muxer_encrypt_init(
	hds_muxer_state_t* state,
	hds_encryption_params_t* encryption_params)
{
	return VOD_UNEXPECTED;
}

static u_char*
hds_muxer_encrypt_write_header(hds_muxer_state_t* state, u_char* p)
{
	return p;
}

static vod_status_t
hds_muxer_encrypt_start_frame(hds_muxer_state_t* state)
{
	return VOD_UNEXPECTED;
}

static vod_status_t
hds_muxer_encrypt_write(
	hds_muxer_state_t* state, 
	u_char* buffer, 
	uint32_t size, 
	bool_t frame_done)
{
	return VOD_UNEXPECTED;
}
#endif //(VOD_HAVE_OPENSSL_EVP)

////// Stateless write functions

static vod_status_t
hds_get_sound_info(request_context_t* request_context, media_info_t* media_info, uint8_t* result)
{
	int sound_rate;
	int sound_size;
	int sound_type;
	int sound_format;

	if (media_info->u.audio.sample_rate <= 8000)
	{
		sound_rate = SOUND_RATE_5_5_KHZ;
	}
	else if (media_info->u.audio.sample_rate <= 16000)
	{
		sound_rate = SOUND_RATE_11_KHZ;
	}
	else if (media_info->u.audio.sample_rate <= 32000)
	{
		sound_rate = SOUND_RATE_22_KHZ;
	}
	else
	{
		sound_rate = SOUND_RATE_44_KHZ;
	}

	switch (media_info->u.audio.bits_per_sample)
	{
	case 8:
		sound_size = SOUND_SIZE_8_BIT;
		break;
	default:
		sound_size = SOUND_SIZE_16_BIT;
		break;
	}

	switch (media_info->u.audio.channels)
	{
	case 1:
		sound_type = SOUND_TYPE_MONO;
		break;
	default:
		sound_type = SOUND_TYPE_STEREO;
		break;
	}

	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_MP3:
		sound_format = SOUND_FORMAT_MP3;
		break;

	default:
		sound_format = SOUND_FORMAT_AAC;
		break;
	}

	*result = (sound_format << 4) | (sound_rate << 2) | (sound_size << 1) | (sound_type);

	return VOD_OK;
}

static u_char*
hds_write_adobe_mux_packet_header(
	u_char* p, 
	uint8_t tag_type, 
	uint32_t data_size, 
	uint32_t timestamp)
{
	*p++ = tag_type;
	write_be24(p, data_size);
	write_be24(p, timestamp);
	*p++ = timestamp >> 24;
	write_be24(p, 0);		// stream id
	return p;
}

static u_char* 
hds_write_video_tag_header(
	u_char* p, 
	uint8_t tag_type,
	uint32_t data_size, 
	uint32_t timestamp, 
	uint8_t frame_type, 
	uint8_t avc_packet_type, 
	uint32_t comp_time_offset)
{
	data_size += sizeof(video_tag_header_avc);

	p = hds_write_adobe_mux_packet_header(p, tag_type, data_size, timestamp);
	*p++ = (frame_type << 4) | CODEC_ID_AVC;
	*p++ = avc_packet_type;
	write_be24(p, comp_time_offset);
	return p;
}

static u_char*
hds_write_audio_tag_header(
	u_char* p,
	uint8_t tag_type,
	uint32_t data_size,
	uint32_t timestamp,
	uint8_t sound_info)
{
	data_size += sizeof(audio_tag_header);
	p = hds_write_adobe_mux_packet_header(p, tag_type, data_size, timestamp);
	*p++ = sound_info;
	return p;
}

static u_char*
hds_write_audio_tag_header_aac(
	u_char* p,
	uint8_t tag_type,
	uint32_t data_size,
	uint32_t timestamp,
	uint8_t sound_info,
	uint8_t aac_packet_type)
{
	data_size += sizeof(audio_tag_header_aac);
	p = hds_write_adobe_mux_packet_header(p, tag_type, data_size, timestamp);
	*p++ = sound_info;
	*p++ = aac_packet_type;
	return p;
}

static u_char*
hds_write_afra_atom_header(u_char* p, size_t atom_size, uint32_t video_key_frame_count)
{
	write_atom_header(p, atom_size, 'a', 'f', 'r', 'a');
	write_be32(p, 0);
	*p++ = 0xC0;								// LongIDs | LongOffsets
	write_be32(p, HDS_TIMESCALE);				// timescale
	write_be32(p, video_key_frame_count);		// entries
	return p;
}

static u_char*
hds_write_afra_atom_entry(u_char* p, uint64_t time, uint64_t offset)
{
	write_be64(p, time);
	write_be64(p, offset);
	return p;
}

static u_char*
hds_write_tfhd_atom(u_char* p, uint32_t track_id, uint64_t base_data_offset)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(hds_tfhd_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'h', 'd');
	write_be32(p, 3);							// flags - base data offset | sample description
	write_be32(p, track_id);
	write_be64(p, base_data_offset);
	write_be32(p, 1);							// sample_desc_index
	return p;
}

static u_char*
hds_write_single_video_frame_trun_atom(u_char* p, hds_encryption_type_t enc_type, input_frame_t* frame, uint32_t offset)
{
	uint32_t size;
	size_t atom_size;

	size = frame->size;
	if (enc_type != HDS_ENC_NONE)
	{
		size = sizeof(hds_selective_encryption_filter_params_t) +
			aes_round_up_to_block(size);
	}

	atom_size = TRUN_SIZE_SINGLE_VIDEO_FRAME;

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, TRUN_VIDEO_FLAGS);	// flags = data offset, duration, size, key, delay
	write_be32(p, 1);					// frame count
	write_be32(p, offset);				// offset from mdat start to frame raw data (excluding the tag)
	write_be32(p, frame->duration);
	write_be32(p, size);
	if (frame->key_frame)
	{
		write_be32(p, 0x02000000);		// I-frame
	}
	else
	{
		write_be32(p, 0x01010000);		// not I-frame + non key sample
	}
	write_be32(p, frame->pts_delay);
	return p;
}

static u_char*
hds_write_single_audio_frame_trun_atom(u_char* p, hds_encryption_type_t enc_type, input_frame_t* frame, uint32_t offset)
{
	uint32_t size;
	size_t atom_size;

	size = frame->size;
	if (enc_type != HDS_ENC_NONE)
	{
		size = sizeof(hds_selective_encryption_filter_params_t) +
			aes_round_up_to_block(size);
	}

	atom_size = TRUN_SIZE_SINGLE_AUDIO_FRAME;

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, TRUN_AUDIO_FLAGS);	// flags = data offset, duration, size
	write_be32(p, 1);					// frame count
	write_be32(p, offset);				// offset from mdat start to frame raw data (excluding the tag)
	write_be32(p, frame->duration);
	write_be32(p, size);
	return p;
}

static size_t
hds_get_traf_atom_size(hds_muxer_stream_state_t* cur_stream)
{
	size_t result;
	
	result = ATOM_HEADER_SIZE + ATOM_HEADER_SIZE + sizeof(hds_tfhd_atom_t);
	switch (cur_stream->media_type)
	{
	case MEDIA_TYPE_VIDEO:
		result += cur_stream->frame_count * TRUN_SIZE_SINGLE_VIDEO_FRAME;
		break;
	case MEDIA_TYPE_AUDIO:
		result += cur_stream->frame_count * TRUN_SIZE_SINGLE_AUDIO_FRAME;
		break;
	}
	return result;
}

static uint32_t
hds_muxer_get_tag_size(media_track_t* cur_track)
{
	switch (cur_track->media_info.codec_id)
	{
	case VOD_CODEC_ID_MP3:
		return sizeof(adobe_mux_packet_header_t) + sizeof(audio_tag_header);

	case VOD_CODEC_ID_AAC:
		return sizeof(adobe_mux_packet_header_t) + sizeof(audio_tag_header_aac);

	default: // VOD_CODEC_ID_AVC
		return sizeof(adobe_mux_packet_header_t) + sizeof(video_tag_header_avc);
	}
}

////// Muxer

static vod_status_t
hds_muxer_init_track(
	hds_muxer_state_t* state,
	hds_muxer_stream_state_t* cur_stream,
	media_track_t* cur_track)
{
	vod_status_t rc;

	cur_stream->track = cur_track;
	cur_stream->media_type = cur_track->media_info.media_type;
	cur_stream->first_frame_part = &cur_track->frames;
	cur_stream->cur_frame_part = cur_track->frames;
	cur_stream->cur_frame = cur_track->frames.first_frame;
	cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);

	cur_stream->first_frame_time_offset = hds_rescale_millis(cur_track->clip_start_time) + cur_track->first_frame_time_offset;
	cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;

	if (cur_track->media_info.media_type == MEDIA_TYPE_AUDIO)
	{
		rc = hds_get_sound_info(state->request_context, &cur_track->media_info, &cur_stream->sound_info);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hds_muxer_init_track: hds_get_sound_info failed %i", rc);
			return rc;
		}
	}
	else
	{
		cur_stream->sound_info = 0;
	}
	
	cur_stream->tag_size = hds_muxer_get_tag_size(cur_track);

	return VOD_OK;
}

static vod_status_t
hds_muxer_reinit_tracks(hds_muxer_state_t* state)
{
	media_track_t* cur_track;
	hds_muxer_stream_state_t* cur_stream;
	vod_status_t rc;

	state->first_time = TRUE;
	state->codec_config_size = 0;

	cur_track = state->first_clip_track;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_track++)
	{
		rc = hds_muxer_init_track(state, cur_stream, cur_track);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (cur_track->media_info.codec_id != VOD_CODEC_ID_MP3)
		{
			state->codec_config_size +=
				cur_stream->tag_size +
				cur_track->media_info.extra_data.len +
				sizeof(uint32_t);
		}
	}
	state->first_clip_track = cur_track;

	return VOD_OK;
}

static vod_status_t
hds_muxer_choose_stream(hds_muxer_state_t* state, hds_muxer_stream_state_t** result)
{
	hds_muxer_stream_state_t* cur_stream;
	hds_muxer_stream_state_t* min_dts = NULL;
	vod_status_t rc;

	for (;;)
	{
		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			if (cur_stream->cur_frame >= cur_stream->cur_frame_part.last_frame)
			{
				if (cur_stream->cur_frame_part.next == NULL)
				{
					continue;
				}

				cur_stream->cur_frame_part = *cur_stream->cur_frame_part.next;
				cur_stream->cur_frame = cur_stream->cur_frame_part.first_frame;
				cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);
				state->first_time = TRUE;
			}

			if (min_dts == NULL || cur_stream->next_frame_time_offset < min_dts->next_frame_time_offset)
			{
				min_dts = cur_stream;
			}
		}

		if (min_dts != NULL)
		{
			*result = min_dts;
			return VOD_OK;
		}

		if (state->first_clip_track >= state->media_set->filtered_tracks_end)
		{
			break;
		}

		rc = hds_muxer_reinit_tracks(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_NOT_FOUND;
}

static vod_status_t
hds_calculate_output_offsets_and_write_afra_entries(
	hds_muxer_state_t* state, 
	uint32_t initial_value, 
	uint32_t afra_entries_base, 
	size_t* frames_size,
	u_char** p)
{
	hds_muxer_stream_state_t* selected_stream;
	hds_muxer_stream_state_t* cur_stream;
	uint32_t cur_offset = initial_value;
	vod_status_t rc;

	for (;;)
	{
		// choose a stream
		rc = hds_muxer_choose_stream(state, &selected_stream);
		if (rc != VOD_OK)
		{
			if (rc == VOD_NOT_FOUND)
			{
				break;		// done
			}
			return rc;
		}

		// video key frames start with the codec info
		if (selected_stream->cur_frame->key_frame && selected_stream->media_type == MEDIA_TYPE_VIDEO)
		{
			if (p != NULL)
			{
				*p = hds_write_afra_atom_entry(
					*p,
					selected_stream->next_frame_time_offset,
					cur_offset + afra_entries_base);
			}
			cur_offset += state->codec_config_size;
		}

		// skip the tag size
		cur_offset += selected_stream->tag_size;

		if (state->enc_type != HDS_ENC_NONE)
		{
			cur_offset += ENCRYPTION_TAG_HEADER_SIZE;
		}

		// set the offset (points to the beginning of the actual data)
		*selected_stream->cur_frame_output_offset = cur_offset;
		selected_stream->cur_frame_output_offset++;

		// move to the end of the frame
		if (state->enc_type != HDS_ENC_NONE)
		{
			cur_offset += sizeof(hds_selective_encryption_filter_params_t) +
				aes_round_up_to_block(selected_stream->cur_frame->size);
		}
		else
		{
			cur_offset += selected_stream->cur_frame->size;
		}
		cur_offset += sizeof(uint32_t);

		// move to the next frame
		selected_stream->next_frame_time_offset += selected_stream->cur_frame->duration;
		selected_stream->cur_frame++;
	}

	// reset the state
	if (state->media_set->clip_count > 1)
	{
		state->first_clip_track = state->media_set->filtered_tracks;
		rc = hds_muxer_reinit_tracks(state);
		if (rc != VOD_OK)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"hds_calculate_output_offsets_and_write_afra_entries: unexpected - hds_muxer_reinit_tracks failed %i", rc);
			return rc;
		}

		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;
		}
	}
	else
	{
		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			cur_stream->cur_frame_part = *cur_stream->first_frame_part;
			cur_stream->cur_frame = cur_stream->cur_frame_part.first_frame;
			cur_stream->source = get_frame_part_source_clip(cur_stream->cur_frame_part);
			cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;
			cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
		}
	}

	*frames_size = cur_offset - initial_value;

	return VOD_OK;
}

static vod_status_t
hds_muxer_init_state(
	request_context_t* request_context,
	hds_encryption_params_t* encryption_params,
	media_set_t* media_set,
	write_callback_t write_callback,
	void* write_context,
	hds_muxer_state_t** result)
{
	media_track_t* cur_track;
	hds_muxer_stream_state_t* cur_stream;
	hds_muxer_state_t* state;
	uint32_t clip_index;
	uint32_t index;
	vod_status_t rc;

	// allocate the state and stream states
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_state: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->first_stream = vod_alloc(
		request_context->pool, 
		sizeof(state->first_stream[0]) * media_set->total_track_count);
	if (state->first_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_state: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	state->last_stream = state->first_stream + media_set->total_track_count;
	state->request_context = request_context;
	state->cur_frame = NULL;
	state->first_time = TRUE;

	state->media_set = media_set;

	write_buffer_init(&state->write_buffer_state, request_context, write_callback, write_context, FALSE);

	state->codec_config_size = 0;

	index = 0;
	cur_track = media_set->filtered_tracks;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_track++, index++)
	{
		cur_stream->index = index;

		// get total frame count for this stream
		cur_stream->frame_count = cur_track->frame_count;
		for (clip_index = 1; clip_index < media_set->clip_count; clip_index++)
		{
			cur_stream->frame_count += cur_track[clip_index * media_set->total_track_count].frame_count;
		}

		// allocate the output offset
		cur_stream->first_frame_output_offset = vod_alloc(
			request_context->pool,
			cur_stream->frame_count * sizeof(uint32_t));
		if (cur_stream->first_frame_output_offset == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"hds_muxer_init_state: vod_alloc failed (3)");
			return VOD_ALLOC_FAILED;
		}
		cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;

		// init the stream
		rc = hds_muxer_init_track(state, cur_stream, cur_track);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// update the codec config size
		if (cur_track->media_info.codec_id != VOD_CODEC_ID_MP3)
		{
			state->codec_config_size +=
				cur_stream->tag_size +
				cur_track->media_info.extra_data.len +
				sizeof(uint32_t);
		}
	}

	state->first_clip_track = cur_track;

	state->enc_type = encryption_params->type;
	if (state->enc_type != HDS_ENC_NONE)
	{
		rc = hds_muxer_encrypt_init(state, encryption_params);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	else
	{
		state->video_tag_type = TAG_TYPE_VIDEO;
		state->audio_tag_type = TAG_TYPE_AUDIO;
	}

	*result = state;

	return VOD_OK;
}

static u_char*
hds_muxer_write_codec_config(u_char* p, hds_muxer_state_t* state, uint64_t cur_frame_dts)
{
	media_track_t* cur_track;
	hds_muxer_stream_state_t* cur_stream;
	uint32_t packet_size;
	u_char* packet_start;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if ((cur_stream->sound_info >> 4) == SOUND_FORMAT_MP3)
		{
			continue;
		}

		cur_track = cur_stream->track;
		packet_start = p;
		switch (cur_track->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			p = hds_write_video_tag_header(
				p,
				TAG_TYPE_VIDEO,
				cur_track->media_info.extra_data.len,
				cur_frame_dts,
				FRAME_TYPE_KEY_FRAME,
				AVC_PACKET_TYPE_SEQUENCE_HEADER,
				0);
			break;

		case MEDIA_TYPE_AUDIO:
			p = hds_write_audio_tag_header_aac(
				p,
				TAG_TYPE_AUDIO,
				cur_track->media_info.extra_data.len,
				cur_frame_dts,
				cur_stream->sound_info,
				AAC_PACKET_TYPE_SEQUENCE_HEADER);
			break;
		}
		p = vod_copy(p, cur_track->media_info.extra_data.data, cur_track->media_info.extra_data.len);
		packet_size = p - packet_start;
		write_be32(p, packet_size);
	}
	return p;
}

vod_status_t
hds_muxer_init_fragment(
	request_context_t* request_context,
	hds_fragment_config_t* conf,
	hds_encryption_params_t* encryption_params,
	uint32_t segment_index,
	media_set_t* media_set,
	write_callback_t write_callback,
	void* write_context,
	bool_t size_only,
	vod_str_t* header, 
	size_t* total_fragment_size,
	hds_muxer_state_t** processor_state)
{
	media_track_t* cur_track;
	hds_muxer_stream_state_t* cur_stream;
	frame_list_part_t* part;
	media_sequence_t* sequence;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	hds_muxer_state_t* state;
	vod_status_t rc;
	uint32_t video_key_frame_count = 0;
	uint32_t track_id = 1;
	uint32_t clip_index;
	uint32_t* output_offset;
	uint32_t mdat_header_size;
	size_t afra_atom_size;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t mdat_atom_size = 0;
	size_t result_size;
	u_char* p;

	// get the total video key frame count
	for (sequence = media_set->sequences; sequence < media_set->sequences_end; sequence++)
	{
		video_key_frame_count += sequence->video_key_frame_count;
	}

	// initialize the muxer state
	rc = hds_muxer_init_state(
		request_context, 
		encryption_params,
		media_set,
		write_callback, 
		write_context, 
		&state);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_fragment: hds_muxer_init_state failed %i", rc);
		return rc;
	}

	// get the mdat header size
	mdat_header_size = ATOM_HEADER_SIZE;
	if (video_key_frame_count == 0)
	{
		// audio only - output the codec config up front, video - output the codec config before every key frame
		mdat_header_size += state->codec_config_size;
	}

	// get the fragment header size
	if (conf->generate_moof_atom)
	{
		afra_atom_size = ATOM_HEADER_SIZE + sizeof(afra_atom_t) + sizeof(afra_entry_t) * video_key_frame_count;
		moof_atom_size =
			ATOM_HEADER_SIZE +
			ATOM_HEADER_SIZE + sizeof(mfhd_atom_t);

		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			moof_atom_size += hds_get_traf_atom_size(cur_stream);
		}
	}
	else
	{
		afra_atom_size = 0;
		moof_atom_size = 0;
	}

	// if not going to write an afra atom, can get the output offsets now
	if (size_only || !conf->generate_moof_atom)
	{
		rc = hds_calculate_output_offsets_and_write_afra_entries(state, mdat_header_size, 0, &mdat_atom_size, NULL);
		if (rc != VOD_OK)
		{
			return rc;
		}

		mdat_atom_size += mdat_header_size;

		*total_fragment_size =
			afra_atom_size +
			moof_atom_size +
			mdat_atom_size;

		// head request optimization
		if (size_only)
		{
			return VOD_OK;
		}
	}

	// allocate the response
	result_size =
		afra_atom_size +
		moof_atom_size +
		mdat_header_size;

	header->data = vod_alloc(request_context->pool, result_size);
	if (header->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_fragment: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	p = header->data;

	if (conf->generate_moof_atom)
	{
		// afra
		p = hds_write_afra_atom_header(p, afra_atom_size, video_key_frame_count);

		rc = hds_calculate_output_offsets_and_write_afra_entries(
			state, 
			moof_atom_size + mdat_header_size, 
			afra_atom_size, 
			&mdat_atom_size, 
			&p);
		if (rc != VOD_OK)
		{
			return rc;
		}

		mdat_atom_size += mdat_header_size;

		// calculate the total size now that we have the mdat size
		*total_fragment_size =
			afra_atom_size +
			moof_atom_size +
			mdat_atom_size;

		// moof
		write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

		// moof.mfhd
		p = mp4_fragment_write_mfhd_atom(p, segment_index);

		for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
		{
			// moof.traf
			traf_atom_size = hds_get_traf_atom_size(cur_stream);
			write_atom_header(p, traf_atom_size, 't', 'r', 'a', 'f');

			// moof.traf.tfhd
			p = hds_write_tfhd_atom(p, track_id, ATOM_HEADER_SIZE + sizeof(afra_atom_t) + moof_atom_size);

			// moof.traf.trun
			switch (cur_stream->media_type)
			{
			case MEDIA_TYPE_VIDEO:
				clip_index = 0;
				cur_track = media_set->filtered_tracks + cur_stream->index;
				output_offset = cur_stream->first_frame_output_offset;
				for (;;)
				{
					part = &cur_track->frames;
					last_frame = part->last_frame;
					for (cur_frame = part->first_frame; ;
						cur_frame++, output_offset++)
					{
						if (cur_frame >= last_frame)
						{
							if (part->next == NULL)
							{
								break;
							}
							part = part->next;
							cur_frame = part->first_frame;
							last_frame = part->last_frame;
						}

						p = hds_write_single_video_frame_trun_atom(p, state->enc_type, cur_frame, *output_offset);
					}

					clip_index++;
					if (clip_index >= media_set->clip_count)
					{
						break;
					}
					cur_track += media_set->total_track_count;
				}
				break;

			case MEDIA_TYPE_AUDIO:
				clip_index = 0;
				cur_track = media_set->filtered_tracks + cur_stream->index;
				output_offset = cur_stream->first_frame_output_offset;
				for (;;)
				{
					part = &cur_track->frames;
					last_frame = part->last_frame;
					for (cur_frame = part->first_frame; ;
						cur_frame++, output_offset++)
					{
						if (cur_frame >= last_frame)
						{
							if (part->next == NULL)
							{
								break;
							}
							part = part->next;
							cur_frame = part->first_frame;
							last_frame = part->last_frame;
						}

						p = hds_write_single_audio_frame_trun_atom(p, state->enc_type, cur_frame, *output_offset);
					}

					clip_index++;
					if (clip_index >= media_set->clip_count)
					{
						break;
					}
					cur_track += media_set->total_track_count;
				}
				break;
			}
		}
	}

	// mdat
	write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');

	if (video_key_frame_count == 0)
	{
		p = hds_muxer_write_codec_config(
			p, 
			state, 
			state->first_stream->next_frame_time_offset);
	}

	header->len = p - header->data;

	if (header->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"hds_muxer_init_fragment: result length %uz exceeded allocated length %uz",
			header->len, result_size);
		return VOD_UNEXPECTED;
	}

	rc = hds_muxer_start_frame(state);
	if (rc != VOD_OK)
	{
		if (rc == VOD_NOT_FOUND)
		{
			*processor_state = NULL;		// no frames, nothing to do
			return VOD_OK;
		}

		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_fragment: hds_muxer_start_frame failed %i", rc);
		return rc;
	}

	*processor_state = state;
	return VOD_OK;
}

static vod_status_t
hds_muxer_start_frame(hds_muxer_state_t* state)
{
	hds_muxer_stream_state_t* selected_stream;
	hds_muxer_stream_state_t* cur_stream;
	read_cache_hint_t cache_hint;
	input_frame_t* cur_frame;
	uint64_t cur_frame_dts;
	uint32_t frame_size;
	size_t alloc_size;
	u_char* p;
	vod_status_t rc;

	rc = hds_muxer_choose_stream(state, &selected_stream);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// init the frame
	state->cur_frame = selected_stream->cur_frame;
	state->frames_source = selected_stream->cur_frame_part.frames_source;
	state->frames_source_context = selected_stream->cur_frame_part.frames_source_context;
	selected_stream->cur_frame++;
	selected_stream->cur_frame_output_offset++;

	cur_frame_dts = selected_stream->next_frame_time_offset;
	selected_stream->next_frame_time_offset += state->cur_frame->duration;

	state->cache_slot_id = selected_stream->media_type;

	// allocate room for the mux packet header
	alloc_size = selected_stream->tag_size;
	if (selected_stream->media_type == MEDIA_TYPE_VIDEO && state->cur_frame->key_frame)
	{
		alloc_size += state->codec_config_size;
	}

	frame_size = state->cur_frame->size;
	if (state->enc_type != HDS_ENC_NONE)
	{
		frame_size = 
			ENCRYPTION_TAG_HEADER_SIZE +
			sizeof(hds_selective_encryption_filter_params_t) +
			aes_round_up_to_block(frame_size);
		alloc_size += ENCRYPTION_TAG_HEADER_SIZE + sizeof(hds_selective_encryption_filter_params_t);
	}

	state->frame_header_size = selected_stream->tag_size;
	state->frame_size = frame_size;

	rc = write_buffer_get_bytes(&state->write_buffer_state, alloc_size, NULL, &p);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"hds_muxer_start_frame: write_buffer_get_bytes failed %i", rc);
		return rc;
	}

	// write the mux packet header and optionally codec config
	if (selected_stream->media_type == MEDIA_TYPE_VIDEO && state->cur_frame->key_frame)
	{
		p = hds_muxer_write_codec_config(p, state, cur_frame_dts);
	}

	switch (selected_stream->media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = hds_write_video_tag_header(
			p,
			state->video_tag_type,
			frame_size,
			cur_frame_dts,
			state->cur_frame->key_frame ? FRAME_TYPE_KEY_FRAME : FRAME_TYPE_INTER_FRAME,
			AVC_PACKET_TYPE_NALU,
			state->cur_frame->pts_delay);
		break;

	case MEDIA_TYPE_AUDIO:
		if ((selected_stream->sound_info >> 4) == SOUND_FORMAT_AAC)
		{
			p = hds_write_audio_tag_header_aac(
				p,
				state->audio_tag_type,
				frame_size,
				cur_frame_dts,
				selected_stream->sound_info,
				AAC_PACKET_TYPE_RAW);
		}
		else
		{
			p = hds_write_audio_tag_header(
				p,
				state->audio_tag_type,
				frame_size,
				cur_frame_dts,
				selected_stream->sound_info);
		}
	}

	if (state->enc_type != HDS_ENC_NONE)
	{
		p = hds_muxer_encrypt_write_header(state, p);

		rc = hds_muxer_encrypt_start_frame(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// find the min offset
	cache_hint.min_offset = ULLONG_MAX;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream)
		{
			continue;
		}

		cur_frame = cur_stream->cur_frame;
		if (cur_frame < cur_stream->cur_frame_part.last_frame &&
			cur_frame->offset < cache_hint.min_offset &&
			cur_stream->source == selected_stream->source)
		{
			cache_hint.min_offset = cur_frame->offset;
			cache_hint.min_offset_slot_id = cur_stream->media_type;
		}
	}

	rc = state->frames_source->start_frame(state->frames_source_context, state->cur_frame, &cache_hint);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
hds_muxer_end_frame(hds_muxer_state_t* state)
{
	uint32_t packet_size = state->frame_header_size + state->frame_size;
	vod_status_t rc;
	u_char* p;

	// write the frame size
	rc = write_buffer_get_bytes(&state->write_buffer_state, sizeof(uint32_t), NULL, &p);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"hds_muxer_end_frame: write_buffer_get_bytes failed %i", rc);
		return rc;
	}
	write_be32(p, packet_size);

	return VOD_OK;
}

vod_status_t
hds_muxer_process_frames(hds_muxer_state_t* state)
{
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;
	bool_t wrote_data = FALSE;
	bool_t frame_done;

	for (;;)
	{
		// read some data from the frame
		rc = state->frames_source->read(state->frames_source_context, &read_buffer, &read_size, &frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!wrote_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"hds_muxer_process_frames: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;

			return VOD_AGAIN;
		}

		wrote_data = TRUE;

		// write the frame
		if (state->enc_type != HDS_ENC_NONE)
		{
			rc = hds_muxer_encrypt_write(state, read_buffer, read_size, frame_done);
		}
		else
		{
			rc = write_buffer_write(&state->write_buffer_state, read_buffer, read_size);
		}

		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hds_muxer_process_frames: write buffer failed %i", rc);
			return rc;
		}

		// if not done, ask the cache for more data
		if (!frame_done)
		{
			continue;
		}

		// end the frame and start a new one
		rc = hds_muxer_end_frame(state);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hds_muxer_process_frames: write_buffer_write failed %i", rc);
			return rc;
		}

		rc = hds_muxer_start_frame(state);
		if (rc != VOD_OK)
		{
			if (rc == VOD_NOT_FOUND)
			{
				break;		// done
			}

			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hds_muxer_process_frames: hds_muxer_start_frame failed %i", rc);
			return rc;
		}
	}

	// flush the buffer
	rc = write_buffer_flush(&state->write_buffer_state, FALSE);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"hds_muxer_process_frames: write_buffer_flush failed %i", rc);
		return rc;
	}
	return VOD_OK;
}
