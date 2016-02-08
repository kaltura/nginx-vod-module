#ifndef __MP4_ENCRYPT_H__
#define __MP4_ENCRYPT_H__

// includes
#include "../dynamic_buffer.h"
#include "../write_buffer.h"
#include "../media_set.h"
#include "mp4_aes_ctr.h"

// constants
#define MP4_ENCRYPT_KEY_SIZE (16)
#define MP4_ENCRYPT_KID_SIZE (16)
#define MP4_ENCRYPT_SYSTEM_ID_SIZE (16)

#define VOD_GUID_LENGTH (sizeof("00000000-0000-0000-0000-000000000000") - 1)

// typedef
struct mp4_encrypt_video_state_s;
typedef struct mp4_encrypt_video_state_s mp4_encrypt_video_state_t;

typedef vod_status_t (*mp4_encrypt_video_write_fragment_header_t)(mp4_encrypt_video_state_t* state);

typedef struct {
	u_char system_id[MP4_ENCRYPT_SYSTEM_ID_SIZE];
	vod_str_t data;
} mp4_encrypt_system_info_t;

typedef struct {
	uint32_t count;
	mp4_encrypt_system_info_t* first;
	mp4_encrypt_system_info_t* last;
} mp4_encrypt_system_info_array_t;

typedef struct {
	u_char key_id[MP4_ENCRYPT_KID_SIZE];
	u_char key[MP4_ENCRYPT_KEY_SIZE];
	mp4_encrypt_system_info_array_t pssh_array;
} mp4_encrypt_info_t;

typedef struct {
	segment_writer_t segment_writer;

	// fixed
	request_context_t* request_context;
	media_set_t* media_set;
	media_sequence_t* sequence;
	uint32_t segment_index;

	// write buffer
	write_buffer_state_t write_buffer;

	// encryption state
#if (VOD_HAVE_OPENSSL_EVP)
	mp4_aes_ctr_state_t cipher;
#endif
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
} mp4_encrypt_state_t;

struct mp4_encrypt_video_state_s {
	mp4_encrypt_state_t base;

	// fixed
	mp4_encrypt_video_write_fragment_header_t write_fragment_header;
	uint32_t nal_packet_size_length;

	// auxiliary data state
	vod_dynamic_buf_t auxiliary_data;
	u_char* auxiliary_sample_sizes;
	u_char* auxiliary_sample_sizes_pos;
	uint16_t subsample_count;

	// nal packet state
	int cur_state;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;

	// saiz / saio
	u_char default_auxiliary_sample_size;
	uint32_t saiz_sample_count;
};

// functions
u_char* mp4_encrypt_write_guid(u_char* p, u_char* guid);

vod_status_t mp4_encrypt_video_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	mp4_encrypt_video_write_fragment_header_t write_fragment_header,
	segment_writer_t* segment_writer,
	const u_char* iv);

vod_status_t mp4_encrypt_audio_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv);

u_char* mp4_encrypt_video_write_saiz_saio(mp4_encrypt_video_state_t* state, u_char* p, size_t auxiliary_data_offset);

size_t mp4_encrypt_audio_get_auxiliary_data_size(mp4_encrypt_state_t* state);

u_char* mp4_encrypt_audio_write_auxiliary_data(mp4_encrypt_state_t* state, u_char* p);

u_char* mp4_encrypt_audio_write_saiz_saio(mp4_encrypt_state_t* state, u_char* p, size_t auxiliary_data_offset);

#endif //__MP4_ENCRYPT_H__
