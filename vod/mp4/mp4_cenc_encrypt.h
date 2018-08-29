#ifndef __MP4_CENC_ENCRYPT_H__
#define __MP4_CENC_ENCRYPT_H__

// includes
#include "../dynamic_buffer.h"
#include "../write_buffer.h"
#include "../media_set.h"
#include "mp4_aes_ctr.h"

// constants
#define VOD_GUID_LENGTH (sizeof("00000000-0000-0000-0000-000000000000") - 1)

// typedef
struct mp4_cenc_encrypt_video_state_s;
typedef struct mp4_cenc_encrypt_video_state_s mp4_cenc_encrypt_video_state_t;

typedef vod_status_t (*mp4_cenc_encrypt_video_build_fragment_header_t)(
	mp4_cenc_encrypt_video_state_t* state, 
	vod_str_t* header, 
	size_t* total_fragment_size);

typedef struct {
	// fixed
	segment_writer_t segment_writer;
	request_context_t* request_context;
	media_set_t* media_set;
	media_sequence_t* sequence;
	uint32_t segment_index;

	// write buffer
	write_buffer_state_t write_buffer;

	// encryption state
	mp4_aes_ctr_state_t cipher;
	u_char iv[MP4_AES_CTR_IV_SIZE];

	// frame state
	media_clip_filtered_t* cur_clip;

	frame_list_part_t* cur_frame_part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t frame_size_left;

	// saiz / saio
	size_t saiz_atom_size;
	size_t saio_atom_size;
} mp4_cenc_encrypt_state_t;

struct mp4_cenc_encrypt_video_state_s {
	mp4_cenc_encrypt_state_t base;

	// fixed
	mp4_cenc_encrypt_video_build_fragment_header_t build_fragment_header;
	uint32_t nal_packet_size_length;
	uint32_t codec_id;

	// auxiliary data state
	vod_dynamic_buf_t auxiliary_data;
	u_char* auxiliary_sample_sizes;
	u_char* auxiliary_sample_sizes_pos;
	uint16_t subsample_count;

	// nal packet state
	int cur_state;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;
	bool_t single_nalu_warning_printed;

	// saiz / saio
	u_char default_auxiliary_sample_size;
	uint32_t saiz_sample_count;
};

// functions
u_char* mp4_cenc_encrypt_write_guid(u_char* p, u_char* guid);

vod_status_t mp4_cenc_encrypt_video_get_fragment_writer(
	segment_writer_t* segment_writer,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	bool_t single_nalu_per_frame,
	mp4_cenc_encrypt_video_build_fragment_header_t build_fragment_header,
	const u_char* iv, 
	vod_str_t* fragment_header,
	size_t* total_fragment_size);

vod_status_t mp4_cenc_encrypt_audio_get_fragment_writer(
	segment_writer_t* segment_writer,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	const u_char* iv);

u_char* mp4_cenc_encrypt_video_write_saiz_saio(mp4_cenc_encrypt_video_state_t* state, u_char* p, size_t auxiliary_data_offset);

size_t mp4_cenc_encrypt_audio_get_auxiliary_data_size(mp4_cenc_encrypt_state_t* state);

u_char* mp4_cenc_encrypt_audio_write_auxiliary_data(mp4_cenc_encrypt_state_t* state, u_char* p);

u_char* mp4_cenc_encrypt_audio_write_saiz_saio(mp4_cenc_encrypt_state_t* state, u_char* p, size_t auxiliary_data_offset);

#endif //__MP4_CENC_ENCRYPT_H__
