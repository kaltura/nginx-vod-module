#include "mkv_builder.h"
#include "mkv_defs.h"
#include "../write_stream.h"
#include "../udrm.h"

#if (VOD_HAVE_OPENSSL_EVP)
#include "../mp4/mp4_aes_ctr.h"
#endif // VOD_HAVE_OPENSSL_EVP

// constants
#define EBML_MAX_NUM_SIZE (8)
#define EBML_ID_SIZE (4)
#define EBML_MASTER_SIZE (EBML_ID_SIZE + EBML_MAX_NUM_SIZE)
#define EBML_UINT_SIZE (EBML_ID_SIZE + 1 + EBML_MAX_NUM_SIZE)
#define EBML_FLOAT_SIZE (EBML_ID_SIZE + 1 + 8)

#define MKV_FRAME_HEADER_SIZE_CLEAR (4)					// track_number (1) + timecode (2) + flags (1)
#define MKV_FRAME_HEADER_SIZE_CLEAR_LEAD (5)			// clear header + signal byte (1)
#define MKV_FRAME_HEADER_SIZE_ENCRYPTED (13)		// clear lead header + iv (8)

#define MKV_TIMESCALE (1000)

// macros
#define ebml_string_size(len) (EBML_ID_SIZE + EBML_MAX_NUM_SIZE + len)

// id writing macros
#define write_id8(p, id)			\
	*(p)++ = (id);					\

#define write_id16(p, id)			\
	{								\
	*(p)++ = ((id) >> 8);			\
	*(p)++ = (id) & 0xFF;			\
	}

#define write_id24(p, id)			\
	{								\
	*(p)++ = ((id) >> 16);			\
	*(p)++ = ((id) >> 8) & 0xFF;	\
	*(p)++ = (id) & 0xFF;			\
	}

#define write_id32(p, id)			\
	{								\
	*(p)++ = ((id) >> 24);			\
	*(p)++ = ((id) >> 16) & 0xFF;	\
	*(p)++ = ((id) >> 8) & 0xFF;	\
	*(p)++ = (id) & 0xFF;			\
	}

// optimized number write macros
#define ebml_write_num_1(p, num)	\
	*(p)++ = 0x80 | (num);

#define ebml_write_num_8(p, num)	\
	*(p)++ = 0x1;					\
	*(p)++ = ((num) >> 48) & 0xFF;	\
	*(p)++ = ((num) >> 40) & 0xFF;	\
	*(p)++ = ((num) >> 32) & 0xFF;	\
	*(p)++ = ((num) >> 24) & 0xFF;	\
	*(p)++ = ((num) >> 16) & 0xFF;	\
	*(p)++ = ((num) >> 8) & 0xFF;	\
	*(p)++ = (num) & 0xFF;

// master write macros
#define ebml_master_skip_size(p, size_ptr)		\
	{											\
	(size_ptr) = (p);							\
	(p) += EBML_MAX_NUM_SIZE;					\
	}

#define ebml_master_update_size(p, size_ptr)				\
	{														\
	uint64_t __val = (p) - (size_ptr) - EBML_MAX_NUM_SIZE;	\
	ebml_write_num_8(size_ptr, __val);						\
	}

// typedefs
typedef struct {
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;
	bool_t reuse_buffers;
	uint32_t frame_header_size;
	mkv_encryption_type_t encryption_type;

#if (VOD_HAVE_OPENSSL_EVP)
	write_buffer_state_t write_buffer;
	mp4_aes_ctr_state_t cipher;
	u_char iv[MP4_AES_CTR_IV_SIZE];
#endif // VOD_HAVE_OPENSSL_EVP

	media_sequence_t* sequence;
	media_clip_filtered_t* cur_clip;
	frame_list_part_t* first_frame_part;
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	bool_t first_time;
	bool_t frame_started;
	uint64_t relative_dts;
	uint32_t timescale;
	bool_t key_frame;
	u_char* frame_headers;
} mkv_fragment_writer_state_t;

static uint32_t frame_header_size_by_enc_type[] = {
	MKV_FRAME_HEADER_SIZE_CLEAR,			// MKV_CLEAR
	MKV_FRAME_HEADER_SIZE_CLEAR_LEAD,		// MKV_CLEAR_LEAD
	MKV_FRAME_HEADER_SIZE_ENCRYPTED,	// MKV_ENCRYPTED
};

/*
Header
	EbmlVersion 1
	EbmlReadVersion 1
	EbmlMaxIdLength 4
	EbmlMaxSizeLength 8
	DocType webm
	DocTypeVersion 2
	DocTypeReadVersion 2
*/
static const u_char webm_header[] = {
	0x1a, 0x45, 0xdf, 0xa3, 0x9f, 0x42, 0x86, 0x81,
	0x01, 0x42, 0xf7, 0x81, 0x01, 0x42, 0xf2, 0x81,
	0x04, 0x42, 0xf3, 0x81, 0x08, 0x42, 0x82, 0x84,
	0x77, 0x65, 0x62, 0x6d, 0x42, 0x87, 0x81, 0x04,
	0x42, 0x85, 0x81, 0x02,
};

static vod_str_t mkv_writing_app = vod_string("nginx-vod-module");

static int 
ebml_num_size(uint64_t num)
{
	int result = 0;

	num += 1;		// all ones signifies unknown size
	do
	{
		num >>= 7;
		result++;
	} while (num);

	return result;
}

static int
ebml_uint_size(uint64_t num)
{
	int size;

	for (size = 1; num >>= 8; size++);
	return size;
}

static u_char* 
ebml_write_num(u_char* p, uint64_t num, int size)
{
	int shift;

	if (size == 0)
	{
		size = ebml_num_size(num);
	}

	num |= 1ULL << (size * 7);
	for (shift = (size - 1) * 8; shift >= 0; shift -= 8)
	{
		*p++ = (num >> shift) & 0xFF;
	}

	return p;
}

static u_char*
ebml_write_uint(u_char* p, uint64_t val)
{
	int size = ebml_uint_size(val);
	int shift;

	ebml_write_num_1(p, size);
	for (shift = (size - 1) * 8; shift >= 0; shift -= 8)
	{
		*p++ = (val >> shift) & 0xFF;
	}

	return p;
}

static u_char*
ebml_write_float(u_char* p, double val)
{
	u_char* src = (u_char*)&val + 7;

	ebml_write_num_1(p, 8);
	*p++ = *src--;
	*p++ = *src--;
	*p++ = *src--;
	*p++ = *src--;
	*p++ = *src--;
	*p++ = *src--;
	*p++ = *src--;
	*p++ = *src--;
	return p;
}

static u_char*
ebml_write_string(u_char* p, vod_str_t* str)
{
	p = ebml_write_num(p, str->len, 0);
	p = vod_copy(p, str->data, str->len);
	return p;
}

static u_char*
mkv_write_info(u_char* p, media_track_t* track)
{
	double duration = track->media_info.duration_millis;
	u_char* info_size;

	write_id32(p, MKV_ID_INFO);
	ebml_master_skip_size(p, info_size);

	write_id24(p, MKV_ID_TIMECODESCALE);
	p = ebml_write_uint(p, NANOS_PER_SEC / MKV_TIMESCALE);

	write_id16(p, MKV_ID_DURATION);
	p = ebml_write_float(p, duration);

	write_id16(p, MKV_ID_MUXINGAPP);
	p = ebml_write_string(p, &mkv_writing_app);

	write_id16(p, MKV_ID_WRITINGAPP);
	p = ebml_write_string(p, &mkv_writing_app);

	ebml_master_update_size(p, info_size);
	return p;
}

static size_t
mkv_get_max_info_size()
{
	return EBML_MASTER_SIZE + EBML_UINT_SIZE + EBML_FLOAT_SIZE + ebml_string_size(mkv_writing_app.len) * 2;
}

static u_char*
mkv_write_content_encodings(u_char* p, drm_info_t* drm_info)
{
	u_char* content_encodings_size;
	u_char* content_encoding_size;
	u_char* content_encryption_size;
	u_char* content_enc_aes_settings_size;

	write_id16(p, MKV_ID_CONTENTENCODINGS);
	ebml_master_skip_size(p, content_encodings_size);

	write_id16(p, MKV_ID_CONTENTENCODING);
	ebml_master_skip_size(p, content_encoding_size);

	write_id16(p, MKV_ID_CONTENTENCODINGORDER);
	p = ebml_write_uint(p, 0);

	write_id16(p, MKV_ID_CONTENTENCODINGSCOPE);
	p = ebml_write_uint(p, 1);			// 1 = all frame contents

	write_id16(p, MKV_ID_CONTENTENCODINGTYPE);
	p = ebml_write_uint(p, 1);			// 1 = encryption

	write_id16(p, MKV_ID_CONTENTENCRYPTION);
	ebml_master_skip_size(p, content_encryption_size);

	write_id16(p, MKV_ID_CONTENTENCALGO);
	p = ebml_write_uint(p, 5);			// 5 = AES

	write_id16(p, MKV_ID_CONTENTENCKEYID);
	ebml_write_num_1(p, DRM_KID_SIZE);
	p = vod_copy(p, drm_info->key_id, DRM_KID_SIZE);

	write_id16(p, MKV_ID_CONTENTENCAESSETTINGS);
	ebml_master_skip_size(p, content_enc_aes_settings_size);

	write_id16(p, MKV_ID_AESSETTINGSCIPHERMODE);
	p = ebml_write_uint(p, 1);			// 1 = CTR

	ebml_master_update_size(p, content_enc_aes_settings_size);
	ebml_master_update_size(p, content_encryption_size);
	ebml_master_update_size(p, content_encoding_size);
	ebml_master_update_size(p, content_encodings_size);
	return p;
}

static size_t
mkv_get_max_content_encodings_size()
{
	return 4 * EBML_MASTER_SIZE + 5 * EBML_UINT_SIZE +
		EBML_MAX_NUM_SIZE + EBML_ID_SIZE + DRM_KID_SIZE;
}

static u_char*
mkv_write_track_video(u_char* p, media_track_t* track)
{
	u_char* video_size;

	write_id8(p, MKV_ID_TRACKVIDEO);
	ebml_master_skip_size(p, video_size);

	write_id8(p, MKV_ID_VIDEOPIXELWIDTH);
	p = ebml_write_uint(p, track->media_info.u.video.width);

	write_id8(p, MKV_ID_VIDEOPIXELHEIGHT);
	p = ebml_write_uint(p, track->media_info.u.video.height);

	ebml_master_update_size(p, video_size);
	return p;
}

static size_t
mkv_get_max_track_video_size()
{
	return EBML_MASTER_SIZE + 2 * EBML_UINT_SIZE;
}

static u_char*
mkv_write_track_audio(u_char* p, media_track_t* track)
{
	u_char* audio_size;

	write_id8(p, MKV_ID_TRACKAUDIO);
	ebml_master_skip_size(p, audio_size);

	write_id8(p, MKV_ID_AUDIOSAMPLINGFREQ);
	p = ebml_write_float(p, track->media_info.u.audio.sample_rate);

	write_id8(p, MKV_ID_AUDIOCHANNELS);
	p = ebml_write_uint(p, track->media_info.u.audio.channels);

	if (track->media_info.u.audio.bits_per_sample != 0)
	{
		write_id16(p, MKV_ID_AUDIOBITDEPTH);
		p = ebml_write_uint(p, track->media_info.u.audio.bits_per_sample);
	}

	ebml_master_update_size(p, audio_size);
	return p;
}

static size_t
mkv_get_max_track_audio_size()
{
	return EBML_MASTER_SIZE + EBML_FLOAT_SIZE + 2 * EBML_UINT_SIZE;
}

static u_char*
mkv_write_track(u_char* p, media_track_t* track, uint64_t track_uid)
{
	mkv_codec_type_t* cur_codec;
	u_char* track_size;

	write_id8(p, MKV_ID_TRACKENTRY);
	ebml_master_skip_size(p, track_size);

	write_id8(p, MKV_ID_TRACKNUMBER);
	p = ebml_write_uint(p, 1);			// can use fixed '1' since the tracks are not muxed together

	write_id16(p, MKV_ID_TRACKUID);
	p = ebml_write_uint(p, track_uid);

	for (cur_codec = mkv_codec_types; cur_codec->mkv_codec_id.len; cur_codec++)
	{
		if (cur_codec->codec_id == track->media_info.codec_id)
		{
			write_id8(p, MKV_ID_TRACKCODECID);
			p = ebml_write_string(p, &cur_codec->mkv_codec_id);
			break;
		}
	}

	if (track->media_info.codec_delay != 0)
	{
		write_id16(p, MKV_ID_TRACKCODECDELAY);
		p = ebml_write_uint(p, track->media_info.codec_delay);
	}
	
	if (track->media_info.extra_data.len != 0)
	{
		write_id16(p, MKV_ID_TRACKCODECPRIVATE);
		p = ebml_write_string(p, &track->media_info.extra_data);
	}

	write_id8(p, MKV_ID_TRACKTYPE);
	switch (track->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = ebml_write_uint(p, MKV_TRACK_TYPE_VIDEO);
		p = mkv_write_track_video(p, track);
		break;

	case MEDIA_TYPE_AUDIO:
		p = ebml_write_uint(p, MKV_TRACK_TYPE_AUDIO);
		p = mkv_write_track_audio(p, track);
		break;
	}

	if (track->file_info.drm_info != NULL)
	{
		p = mkv_write_content_encodings(p, (drm_info_t*)track->file_info.drm_info);
	}

	ebml_master_update_size(p, track_size);
	return p;
}

static size_t
mkv_get_max_track_size(media_track_t* track)
{
	size_t result;

	result = EBML_MASTER_SIZE + 4 * EBML_UINT_SIZE +
		ebml_string_size(MKV_MAX_CODEC_SIZE) + ebml_string_size(track->media_info.extra_data.len) + 
		mkv_get_max_content_encodings_size();

	switch (track->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		result += mkv_get_max_track_video_size();
		break;

	case MEDIA_TYPE_AUDIO:
		result += mkv_get_max_track_audio_size();
		break;
	}

	return result;
}

static u_char*
mkv_write_tracks(u_char* p, media_track_t* track, uint64_t track_uid)
{
	u_char* tracks_size;

	write_id32(p, MKV_ID_TRACKS);
	ebml_master_skip_size(p, tracks_size);

	p = mkv_write_track(p, track, track_uid);

	ebml_master_update_size(p, tracks_size);
	return p;
}

static size_t
mkv_get_max_tracks_size(media_track_t* track)
{
	return EBML_MASTER_SIZE + mkv_get_max_track_size(track);
}

static u_char*
mkv_write_init_segment(u_char* p, media_track_t* track, uint64_t track_uid)
{
	u_char* segment_size;

	p = vod_copy(p, webm_header, sizeof(webm_header));

	write_id32(p, MKV_ID_SEGMENT);
	ebml_master_skip_size(p, segment_size);

	p = mkv_write_info(p, track);

	p = mkv_write_tracks(p, track, track_uid);

	ebml_master_update_size(p, segment_size);
	return p;
}

static size_t
mkv_get_max_init_segment_size(media_track_t* track)
{
	return sizeof(webm_header) + 
		EBML_MASTER_SIZE + 
		mkv_get_max_info_size() + 
		mkv_get_max_tracks_size(track);
}

vod_status_t 
mkv_build_init_segment(
	request_context_t* request_context,
	media_track_t* track,
	uint64_t track_uid,
	vod_str_t* result)
{
	size_t alloc_size;
	u_char* p;
	
	alloc_size = mkv_get_max_init_segment_size(track);

	p = vod_alloc(request_context->pool, alloc_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_build_init_segment: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->data = p;

	p = mkv_write_init_segment(p, track, track_uid);

	result->len = p - result->data;

	if (result->len > alloc_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_build_init_segment: result length %uz greater than allocated length %uz",
			result->len, alloc_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

static void
mkv_builder_init_track(mkv_fragment_writer_state_t* state, media_track_t* track)
{
	state->first_time = TRUE;
	state->first_frame_part = &track->frames;
	state->cur_frame_part = track->frames;
	state->cur_frame = track->frames.first_frame;
	state->timescale = track->media_info.timescale;
	state->key_frame = (track->media_info.media_type == MEDIA_TYPE_AUDIO);

	if (!state->reuse_buffers)
	{
		state->cur_frame_part.frames_source->disable_buffer_reuse(
			state->cur_frame_part.frames_source_context);
	}
}

vod_status_t 
mkv_builder_frame_writer_init(
	request_context_t* request_context,
	media_sequence_t* sequence,
	write_callback_t write_callback,
	void* write_context, 
	bool_t reuse_buffers,
	mkv_encryption_type_t encryption_type,
	u_char* iv,
	vod_str_t* response_header,
	size_t* total_fragment_size,
	void** result)
{
	mkv_fragment_writer_state_t* state;
	media_clip_filtered_t* cur_clip;
	frame_list_part_t* part;
	media_track_t* first_track;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t cluster_timecode;
	uint64_t first_frame_pts_delay;
	size_t block_data_size;
	size_t cluster_size;
	size_t alloc_size;
	size_t 	frame_headers_size;
	uint32_t frame_header_size;
	u_char* p;
#if (VOD_HAVE_OPENSSL_EVP)
	vod_status_t rc;
#endif // VOD_HAVE_OPENSSL_EVP

	frame_header_size = frame_header_size_by_enc_type[encryption_type];

	// calculate the total frame headers size
	frame_headers_size = 0;
	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		part = &cur_clip->first_track->frames;
		last_frame = part->last_frame;
		for (cur_frame = part->first_frame; ; cur_frame++)
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

			block_data_size = frame_header_size + cur_frame->size;
			frame_headers_size += 
				1 + ebml_num_size(block_data_size) +			// simple block
				frame_header_size;
		}
	}

	// calculate the cluster timecode
	first_track = sequence->filtered_clips[0].first_track;

	if (first_track->frame_count > 0)
	{
		first_frame_pts_delay = first_track->frames.first_frame[0].pts_delay;
	}
	else
	{
		first_frame_pts_delay = 0;
	}

	cluster_timecode =
		rescale_time(first_track->first_frame_time_offset + first_frame_pts_delay, first_track->media_info.timescale, MKV_TIMESCALE) +
		first_track->clip_start_time;

	// get the total fragment size
	cluster_size =
		1 + 1 + ebml_uint_size(cluster_timecode) +			// cluster_timecode - id, size, value
		frame_headers_size +
		sequence->total_frame_size;

	*total_fragment_size =
		4 +			// cluster id
		ebml_num_size(cluster_size) +
		cluster_size;

	// build the fragment header
	alloc_size = *total_fragment_size - frame_headers_size - sequence->total_frame_size;

	p = vod_alloc(request_context->pool, alloc_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_builder_frame_writer_init: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	response_header->data = p;

	write_id32(p, MKV_ID_CLUSTER);
	p = ebml_write_num(p, cluster_size, 0);

	write_id8(p, MKV_ID_CLUSTERTIMECODE);
	p = ebml_write_uint(p, cluster_timecode);

	response_header->len = p - response_header->data;

	if (alloc_size != response_header->len)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_builder_frame_writer_init: response header size %uz different than allocated size %uz", 
			response_header->len, alloc_size);
		return VOD_UNEXPECTED;
	}

	// initialize the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_builder_frame_writer_init: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

#if (VOD_HAVE_OPENSSL_EVP)
	if (encryption_type == MKV_ENCRYPTED)
	{
		// init the aes ctr
		rc = mp4_aes_ctr_init(&state->cipher, request_context, ((drm_info_t*)sequence->drm_info)->key);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// init the output buffer
		write_buffer_init(
			&state->write_buffer,
			request_context,
			write_callback,
			write_context,
			reuse_buffers);

		state->reuse_buffers = TRUE;
		vod_memcpy(state->iv, iv, sizeof(state->iv));
	}
	else
#endif // VOD_HAVE_OPENSSL_EVP
	{
		state->frame_headers = vod_alloc(request_context->pool, frame_headers_size);
		if (state->frame_headers == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_builder_frame_writer_init: vod_alloc failed (3)");
			return VOD_ALLOC_FAILED;
		}

		state->write_callback = write_callback;
		state->write_context = write_context;
		state->reuse_buffers = reuse_buffers;
	}

	state->request_context = request_context;
	state->frame_header_size = frame_header_size;
	state->encryption_type = encryption_type;
	state->frame_started = FALSE;
	state->sequence = sequence;
	state->cur_clip = sequence->filtered_clips;
	state->relative_dts = 0;

	mkv_builder_init_track(state, state->cur_clip->first_track);

	*result = state;
	return VOD_OK;
}

static u_char*
mkv_builder_write_clear_frame_header(
	u_char* p, 
	size_t data_size, 
	uint16_t timecode, 
	uint32_t key_frame)
{
	write_id8(p, MKV_ID_SIMPLEBLOCK);
	p = ebml_write_num(p, data_size, 0);

	ebml_write_num_1(p, 1);		// track number
	write_be16(p, timecode);
	*p++ = key_frame ? 0x80 : 0;		// flags

	return p;
}

static vod_status_t
mkv_builder_write_frame_header(mkv_fragment_writer_state_t* state)
{
	input_frame_t* cur_frame = state->cur_frame;
	size_t data_size = state->frame_header_size + cur_frame->size;
	uint64_t relative_pts = state->relative_dts + cur_frame->pts_delay;
	uint16_t timecode = rescale_time(relative_pts, state->timescale, MKV_TIMESCALE);
	u_char* p;
	vod_status_t rc;

#if (VOD_HAVE_OPENSSL_EVP)
	if (state->encryption_type == MKV_ENCRYPTED)
	{
		// write to write_buffer
		rc = write_buffer_get_bytes(
			&state->write_buffer, 
			1 + ebml_num_size(data_size) + MKV_FRAME_HEADER_SIZE_ENCRYPTED, 
			NULL, 
			&p);
		if (rc != VOD_OK)
		{
			return rc;
		}

		p = mkv_builder_write_clear_frame_header(
			p, 
			data_size, 
			timecode, 
			cur_frame->key_frame || state->key_frame);

		*p++ = 0x01;	// encrypted
		p = vod_copy(p, state->iv, MP4_AES_CTR_IV_SIZE);

		mp4_aes_ctr_set_iv(&state->cipher, state->iv);
		mp4_aes_ctr_increment_be64(state->iv);
	}
	else
#endif // VOD_HAVE_OPENSSL_EVP
	{
		// write to frame_headers
		p = state->frame_headers;

		p = mkv_builder_write_clear_frame_header(
			p, 
			data_size, 
			timecode, 
			cur_frame->key_frame || state->key_frame);

		if (state->encryption_type == MKV_CLEAR_LEAD)
		{
			*p++ = 0x00;	// clear
		}

		rc = state->write_callback(state->write_context, state->frame_headers, p - state->frame_headers);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->frame_headers = p;
	}

	state->relative_dts += cur_frame->duration;

	return VOD_OK;
}

static vod_status_t
mkv_builder_start_frame(mkv_fragment_writer_state_t* state)
{
	vod_status_t rc;

	while (state->cur_frame >= state->cur_frame_part.last_frame)
	{
		if (state->cur_frame_part.next != NULL)
		{
			state->cur_frame_part = *state->cur_frame_part.next;
			state->cur_frame = state->cur_frame_part.first_frame;
			state->first_time = TRUE;
			break;
		}

		state->cur_clip++;
		if (state->cur_clip >= state->sequence->filtered_clips_end)
		{
#if (VOD_HAVE_OPENSSL_EVP)
			if (state->encryption_type == MKV_ENCRYPTED)
			{
				rc = write_buffer_flush(&state->write_buffer, FALSE);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
#endif // VOD_HAVE_OPENSSL_EVP

			return VOD_OK;
		}

		mkv_builder_init_track(state, state->cur_clip->first_track);
	}

	rc = mkv_builder_write_frame_header(state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = state->cur_frame_part.frames_source->start_frame(
		state->cur_frame_part.frames_source_context,
		state->cur_frame, 
		NULL);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_AGAIN;
}

vod_status_t
mkv_builder_frame_writer_process(void* context)
{
	mkv_fragment_writer_state_t* state = context;
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;
	bool_t processed_data = FALSE;
	bool_t frame_done;

	if (!state->frame_started)
	{
		rc = mkv_builder_start_frame(state);
		if (rc != VOD_AGAIN)
		{
			return rc;
		}

		state->frame_started = TRUE;
	}

	for (;;)
	{
		// read some data from the frame
		rc = state->cur_frame_part.frames_source->read(
			state->cur_frame_part.frames_source_context, 
			&read_buffer, 
			&read_size, 
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!processed_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mkv_builder_frame_writer_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}

		processed_data = TRUE;

#if (VOD_HAVE_OPENSSL_EVP)
		if (state->encryption_type == MKV_ENCRYPTED)
		{
			rc = mp4_aes_ctr_write_encrypted(
				&state->cipher,
				&state->write_buffer,
				read_buffer,
				read_size);
		}
		else
#endif // VOD_HAVE_OPENSSL_EVP
		{
			rc = state->write_callback(state->write_context, read_buffer, read_size);
		}

		if (rc != VOD_OK)
		{
			return rc;
		}

		if (!frame_done)
		{
			continue;
		}

		// move to the next frame
		state->cur_frame++;

		rc = mkv_builder_start_frame(state);
		if (rc != VOD_AGAIN)
		{
			return rc;
		}
	}
}
