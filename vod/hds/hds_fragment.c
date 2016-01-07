#include "hds_fragment.h"
#include "hds_amf0_encoder.h"
#include "../write_buffer.h"

// adobe mux packet definitions
#define TAG_TYPE_AUDIO (8)
#define TAG_TYPE_VIDEO (9)

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

#define TRUN_SIZE_SINGLE_VIDEO_FRAME (ATOM_HEADER_SIZE + sizeof(trun_atom_t) + 4 * sizeof(uint32_t))
#define TRUN_SIZE_SINGLE_AUDIO_FRAME (ATOM_HEADER_SIZE + sizeof(trun_atom_t) + 2 * sizeof(uint32_t))

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
} tfhd_atom_t;

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
	u_char aac_packet_type[1];
} audio_tag_header_aac;

// state
typedef struct {
	media_track_t* track;
	int media_type;
	uint8_t sound_info;
	uint32_t timescale;
	frames_source_t* frames_source;
	void* frames_source_context;
	uint32_t frame_count;
	uint32_t index;

	uint64_t clip_start_time;
	uint64_t first_frame_time_offset;
	uint64_t next_frame_time_offset;
	uint64_t next_frame_dts;

	// input frames
	input_frame_t* first_frame;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;

	// frame input offsets
	uint64_t* first_frame_input_offset;
	uint64_t* cur_frame_input_offset;

	// frame output offsets
	uint32_t* first_frame_output_offset;
	uint32_t* cur_frame_output_offset;
} hds_muxer_stream_state_t;

struct hds_muxer_state_s {
	request_context_t* request_context;

	hds_muxer_stream_state_t* first_stream;
	hds_muxer_stream_state_t* last_stream;
	uint32_t codec_config_size;

	write_buffer_state_t write_buffer_state;

	// cur sequence state
	media_clip_filtered_t* clips_start;
	media_clip_filtered_t* clips_end;
	media_clip_filtered_t* cur_clip;

	input_frame_t* cur_frame;
	int cache_slot_id;
	frames_source_t* frames_source;
	void* frames_source_context;
	bool_t first_time;

	uint32_t frame_header_size;
};

// constants
static const uint32_t tag_size_by_media_type[MEDIA_TYPE_COUNT] = {
	sizeof(adobe_mux_packet_header_t) + sizeof(video_tag_header_avc),
	sizeof(adobe_mux_packet_header_t) + sizeof(audio_tag_header_aac),
};

static vod_status_t
hds_get_sound_info(request_context_t* request_context, media_info_t* media_info, uint8_t* result)
{
	int sound_rate;
	int sound_size;
	int sound_type;

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

	*result = (SOUND_FORMAT_AAC << 4) | (sound_rate << 2) | (sound_size << 1) | (sound_type);

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
	uint32_t data_size, 
	uint32_t timestamp, 
	uint8_t frame_type, 
	uint8_t avc_packet_type, 
	uint32_t comp_time_offset)
{
	data_size += sizeof(video_tag_header_avc);

	p = hds_write_adobe_mux_packet_header(p, TAG_TYPE_VIDEO, data_size, timestamp);
	*p++ = (frame_type << 4) | CODEC_ID_AVC;
	*p++ = avc_packet_type;
	write_be24(p, comp_time_offset);
	return p;
}

static u_char*
hds_write_audio_tag_header(
	u_char* p,
	uint32_t data_size,
	uint32_t timestamp,
	uint8_t sound_info,
	uint8_t aac_packet_type)
{
	data_size += sizeof(audio_tag_header_aac);

	p = hds_write_adobe_mux_packet_header(p, TAG_TYPE_AUDIO, data_size, timestamp);
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
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfhd_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'h', 'd');
	write_be32(p, 3);							// flags - base data offset | sample description
	write_be32(p, track_id);
	write_be64(p, base_data_offset);
	write_be32(p, 1);							// sample_desc_index
	return p;
}

static u_char*
hds_write_single_video_frame_trun_atom(u_char* p, input_frame_t* frame, uint32_t offset)
{
	size_t atom_size;

	atom_size = TRUN_SIZE_SINGLE_VIDEO_FRAME;

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, 0xF01);				// flags = data offset, duration, size, key, delay
	write_be32(p, 1);					// frame count
	write_be32(p, offset);				// offset from mdat start to frame raw data (excluding the tag)
	write_be32(p, frame->duration);
	write_be32(p, frame->size);
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
hds_write_single_audio_frame_trun_atom(u_char* p, input_frame_t* frame, uint32_t offset)
{
	size_t atom_size;

	atom_size = TRUN_SIZE_SINGLE_AUDIO_FRAME;

	write_atom_header(p, atom_size, 't', 'r', 'u', 'n');
	write_be32(p, 0x301);				// flags = data offset, duration, size
	write_be32(p, 1);					// frame count
	write_be32(p, offset);				// offset from mdat start to frame raw data (excluding the tag)
	write_be32(p, frame->duration);
	write_be32(p, frame->size);
	return p;
}

static size_t
hds_get_traf_atom_size(hds_muxer_stream_state_t* cur_stream)
{
	size_t result;
	
	result = ATOM_HEADER_SIZE + ATOM_HEADER_SIZE + sizeof(tfhd_atom_t);
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

static vod_status_t
hds_muxer_init_track(
	hds_muxer_state_t* state,
	hds_muxer_stream_state_t* cur_stream,
	media_track_t* cur_track)
{
	vod_status_t rc;

	cur_stream->track = cur_track;
	cur_stream->media_type = cur_track->media_info.media_type;
	cur_stream->timescale = cur_track->media_info.timescale;
	cur_stream->frames_source = cur_track->frames_source;
	cur_stream->frames_source_context = cur_track->frames_source_context;
	cur_stream->first_frame = cur_track->first_frame;
	cur_stream->last_frame = cur_track->last_frame;

	cur_stream->clip_start_time = hds_rescale_millis(cur_track->clip_start_time);
	cur_stream->first_frame_time_offset = cur_track->first_frame_time_offset;
	cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
	cur_stream->next_frame_dts = rescale_time(cur_stream->next_frame_time_offset, cur_stream->timescale, HDS_TIMESCALE);

	cur_stream->cur_frame = cur_stream->first_frame;

	cur_stream->first_frame_input_offset = cur_track->frame_offsets;
	cur_stream->cur_frame_input_offset = cur_stream->first_frame_input_offset;

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

	cur_track = state->cur_clip->first_track;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_track++)
	{
		rc = hds_muxer_init_track(state, cur_stream, cur_track);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->codec_config_size += 
			tag_size_by_media_type[cur_track->media_info.media_type] +
			cur_track->media_info.extra_data_size + 
			sizeof(uint32_t);
	}
	state->cur_clip++;

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
			if (cur_stream->cur_frame >= cur_stream->last_frame)
			{
				continue;
			}

			if (min_dts == NULL || cur_stream->next_frame_dts < min_dts->next_frame_dts)
			{
				min_dts = cur_stream;
			}
		}

		if (min_dts != NULL)
		{
			*result = min_dts;
			return VOD_OK;
		}

		if (state->cur_clip >= state->clips_end)
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
		if (selected_stream->cur_frame->key_frame && selected_stream->media_type == MEDIA_TYPE_VIDEO && p != NULL)
		{
			*p = hds_write_afra_atom_entry(
				*p, 
				selected_stream->next_frame_dts + selected_stream->clip_start_time, 
				cur_offset + afra_entries_base);
			cur_offset += state->codec_config_size;
		}

		// skip the tag size
		cur_offset += tag_size_by_media_type[selected_stream->media_type];

		// set the offset (points to the beginning of the actual data)
		*selected_stream->cur_frame_output_offset = cur_offset;
		selected_stream->cur_frame_output_offset++;

		// move to the end of the frame
		cur_offset += selected_stream->cur_frame->size;
		cur_offset += sizeof(uint32_t);

		// move to the next frame
		selected_stream->next_frame_time_offset += selected_stream->cur_frame->duration;
		selected_stream->next_frame_dts = rescale_time(selected_stream->next_frame_time_offset, selected_stream->timescale, HDS_TIMESCALE);
		selected_stream->cur_frame++;
	}

	// reset the state
	if (state->clips_end > state->clips_start + 1)
	{
		state->cur_clip = state->clips_start;
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
			cur_stream->cur_frame = cur_stream->first_frame;
			cur_stream->cur_frame_output_offset = cur_stream->first_frame_output_offset;
			cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
			cur_stream->next_frame_dts = rescale_time(cur_stream->next_frame_time_offset, cur_stream->timescale, HDS_TIMESCALE);
		}
	}

	return VOD_OK;
}

static vod_status_t
hds_muxer_init_state(
	request_context_t* request_context,
	media_sequence_t* sequence,
	write_callback_t write_callback,
	void* write_context,
	hds_muxer_state_t** result)
{
	media_clip_filtered_t* track_array;
	media_track_t* cur_track;
	hds_muxer_stream_state_t* cur_stream;
	hds_muxer_state_t* state;
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
		sizeof(state->first_stream[0]) * sequence->total_track_count);
	if (state->first_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_state: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	state->last_stream = state->first_stream + sequence->total_track_count;
	state->request_context = request_context;
	state->cur_frame = NULL;
	state->first_time = TRUE;

	state->clips_start = sequence->filtered_clips;
	state->clips_end = sequence->filtered_clips_end;
	state->cur_clip = sequence->filtered_clips + 1;

	write_buffer_init(&state->write_buffer_state, request_context, write_callback, write_context, FALSE);

	state->codec_config_size = 0;

	index = 0;
	cur_track = state->clips_start->first_track;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_track++, index++)
	{
		cur_stream->index = index;

		// get total frame count for this stream
		cur_stream->frame_count = cur_track->frame_count;
		for (track_array = state->cur_clip; track_array < state->clips_end; track_array++)
		{
			cur_stream->frame_count += track_array->first_track[cur_stream->index].frame_count;
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
		state->codec_config_size +=
			tag_size_by_media_type[cur_track->media_info.media_type] +
			cur_track->media_info.extra_data_size +
			sizeof(uint32_t);
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
		cur_track = cur_stream->track;
		packet_start = p;
		switch (cur_track->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			p = hds_write_video_tag_header(
				p,
				cur_track->media_info.extra_data_size,
				cur_frame_dts,
				FRAME_TYPE_KEY_FRAME,
				AVC_PACKET_TYPE_SEQUENCE_HEADER,
				0);
			break;

		case MEDIA_TYPE_AUDIO:
			p = hds_write_audio_tag_header(
				p,
				cur_track->media_info.extra_data_size,
				cur_frame_dts,
				cur_stream->sound_info,
				AAC_PACKET_TYPE_SEQUENCE_HEADER);
			break;
		}
		p = vod_copy(p, cur_track->media_info.extra_data, cur_track->media_info.extra_data_size);
		packet_size = p - packet_start;
		write_be32(p, packet_size);
	}
	return p;
}

vod_status_t
hds_muxer_init_fragment(
	request_context_t* request_context,
	hds_fragment_config_t* conf,
	uint32_t segment_index,
	media_sequence_t* sequence,
	write_callback_t write_callback,
	void* write_context,
	bool_t size_only,
	vod_str_t* header, 
	size_t* total_fragment_size,
	hds_muxer_state_t** processor_state)
{
	media_clip_filtered_t* cur_clip;
	media_track_t* cur_track;
	hds_muxer_stream_state_t* cur_stream;
	input_frame_t* cur_frame;
	input_frame_t* frames_end;
	hds_muxer_state_t* state;
	vod_status_t rc;
	uint32_t track_id = 1;
	uint32_t* output_offset;
	uint32_t frame_metadata_size;
	uint32_t video_key_frame_count;
	uint32_t codec_config_size;
	size_t afra_atom_size;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t mdat_atom_size;
	size_t result_size;
	u_char* p;

	// initialize the muxer state
	rc = hds_muxer_init_state(
		request_context, 
		sequence,
		write_callback, 
		write_context, 
		&state);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_muxer_init_fragment: hds_muxer_init_state failed %i", rc);
		return rc;
	}

	// get moof atom size
	mdat_atom_size = ATOM_HEADER_SIZE;
	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		codec_config_size = 0;
		video_key_frame_count = 0;

		for (cur_track = cur_clip->first_track; cur_track < cur_clip->last_track; cur_track++)
		{
			frame_metadata_size = tag_size_by_media_type[cur_track->media_info.media_type] + sizeof(uint32_t);
			codec_config_size += frame_metadata_size + cur_track->media_info.extra_data_size;

			mdat_atom_size += cur_track->total_frames_size + cur_track->frame_count * frame_metadata_size;

			if (cur_track->media_info.media_type == MEDIA_TYPE_VIDEO)
			{
				video_key_frame_count += cur_track->key_frame_count;
			}
		}

		mdat_atom_size += video_key_frame_count * codec_config_size;
	}

	// get the fragment header size
	if (conf->generate_moof_atom)
	{
		afra_atom_size = ATOM_HEADER_SIZE + sizeof(afra_atom_t) + sizeof(afra_entry_t) * sequence->video_key_frame_count;
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

	result_size =
		afra_atom_size +
		moof_atom_size +
		ATOM_HEADER_SIZE;		// mdat

	// audio only - output the codec config up front, video - output the codec config before every key frame
	if (sequence->video_key_frame_count == 0)
	{
		result_size += state->codec_config_size;
		mdat_atom_size += state->codec_config_size;
	}

	*total_fragment_size =
		afra_atom_size +
		moof_atom_size +
		mdat_atom_size;

	// head request optimization
	if (size_only)
	{
		return VOD_OK;
	}

	// allocate the response
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
		p = hds_write_afra_atom_header(p, afra_atom_size, sequence->video_key_frame_count);

		rc = hds_calculate_output_offsets_and_write_afra_entries(state, ATOM_HEADER_SIZE, afra_atom_size + moof_atom_size, &p);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// moof
		write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

		// moof.mfhd
		p = mp4_builder_write_mfhd_atom(p, segment_index);

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
				for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
				{
					cur_track = cur_clip->first_track + cur_stream->index;
					frames_end = cur_track->last_frame;
					for (cur_frame = cur_track->first_frame, output_offset = cur_stream->first_frame_output_offset;
						cur_frame < frames_end;
						cur_frame++, output_offset++)
					{
						p = hds_write_single_video_frame_trun_atom(p, cur_frame, *output_offset);
					}
				}
				break;

			case MEDIA_TYPE_AUDIO:
				for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
				{
					cur_track = cur_clip->first_track + cur_stream->index;
					frames_end = cur_track->last_frame;
					for (cur_frame = cur_track->first_frame, output_offset = cur_stream->first_frame_output_offset;
						cur_frame < frames_end;
						cur_frame++, output_offset++)
					{
						p = hds_write_single_audio_frame_trun_atom(p, cur_frame, *output_offset);
					}
				}
				break;
			}
		}
	}
	else
	{
		// calculate the output offsets
		rc = hds_calculate_output_offsets_and_write_afra_entries(state, 0, 0, NULL);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// mdat
	write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');

	if (sequence->video_key_frame_count == 0)
	{
		p = hds_muxer_write_codec_config(
			p, 
			state, 
			state->first_stream->next_frame_dts + state->first_stream->clip_start_time);
	}

	header->len = p - header->data;

	if (header->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"hds_muxer_init_fragment: result length %uz exceeded allocated length %uz",
			header->len, result_size);
		return VOD_UNEXPECTED;
	}

	*processor_state = state;

	return VOD_OK;
}

static vod_status_t
hds_muxer_start_frame(hds_muxer_state_t* state)
{
	hds_muxer_stream_state_t* selected_stream;
	uint64_t cur_frame_offset;
	uint64_t cur_frame_dts;
	size_t alloc_size;
	u_char* p;
	vod_status_t rc;

	rc = hds_muxer_choose_stream(state, &selected_stream);
	if (rc != VOD_OK)
	{
		if (rc == VOD_NOT_FOUND)
		{
			return VOD_OK;		// done
		}
		return rc;
	}

	// init the frame
	state->cur_frame = selected_stream->cur_frame;
	state->frames_source = selected_stream->frames_source;
	state->frames_source_context = selected_stream->frames_source_context;
	selected_stream->cur_frame++;
	cur_frame_offset = *selected_stream->cur_frame_input_offset;
	selected_stream->cur_frame_input_offset++;
	selected_stream->cur_frame_output_offset++;

	selected_stream->next_frame_time_offset += state->cur_frame->duration;
	cur_frame_dts = selected_stream->next_frame_dts + selected_stream->clip_start_time;
	selected_stream->next_frame_dts = rescale_time(selected_stream->next_frame_time_offset, selected_stream->timescale, HDS_TIMESCALE);

	state->cache_slot_id = selected_stream->media_type;

	// allocate room for the mux packet header
	state->frame_header_size = tag_size_by_media_type[selected_stream->media_type];

	alloc_size = state->frame_header_size;
	if (selected_stream->media_type == MEDIA_TYPE_VIDEO && state->cur_frame->key_frame)
	{
		alloc_size += state->codec_config_size;
	}

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
		hds_write_video_tag_header(
			p,
			state->cur_frame->size,
			cur_frame_dts,
			state->cur_frame->key_frame ? FRAME_TYPE_KEY_FRAME : FRAME_TYPE_INTER_FRAME,
			AVC_PACKET_TYPE_NALU,
			rescale_time(state->cur_frame->pts_delay, selected_stream->timescale, HDS_TIMESCALE));
		break;

	case MEDIA_TYPE_AUDIO:
		hds_write_audio_tag_header(
			p,
			state->cur_frame->size,
			cur_frame_dts,
			selected_stream->sound_info,
			AAC_PACKET_TYPE_RAW);
	}

	rc = state->frames_source->start_frame(state->frames_source_context, state->cur_frame, cur_frame_offset);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
hds_muxer_end_frame(hds_muxer_state_t* state)
{
	uint32_t packet_size = state->frame_header_size + state->cur_frame->size;
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
		// start a new frame if we don't have a frame
		if (state->cur_frame == NULL)
		{
			rc = hds_muxer_start_frame(state);
			if (rc != VOD_OK)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
					"hds_muxer_process_frames: hds_muxer_start_frame failed %i", rc);
				return rc;
			}

			if (state->cur_frame == NULL)
			{
				break;		// done
			}
		}

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
		rc = write_buffer_write(&state->write_buffer_state, read_buffer, read_size);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hds_muxer_process_frames: write_buffer_write failed %i", rc);
			return rc;
		}

		// flush the frame if we finished writing it
		if (frame_done)
		{
			rc = hds_muxer_end_frame(state);
			if (rc != VOD_OK)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
					"hds_muxer_process_frames: write_buffer_write failed %i", rc);
				return rc;
			}

			state->cur_frame = NULL;
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
