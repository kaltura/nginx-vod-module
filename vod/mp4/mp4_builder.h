#ifndef __MP4_BUILDER_H__
#define __MP4_BUILDER_H__

// includes
#include "../read_cache.h"
#include "mp4_parser.h"

// macros
#define write_word(p, w)			\
	{								\
	*(p)++ = ((w) >> 8) & 0xFF;		\
	*(p)++ = (w) & 0xFF;			\
	}

#define write_dword(p, dw)			\
	{								\
	*(p)++ = ((dw) >> 24) & 0xFF;	\
	*(p)++ = ((dw) >> 16) & 0xFF;	\
	*(p)++ = ((dw) >> 8) & 0xFF;	\
	*(p)++ = (dw) & 0xFF;			\
	}

#define write_qword(p, qw)			\
	{								\
	write_dword(p, (qw) >> 32);		\
	write_dword(p, (qw));			\
	}

#define write_atom_name(p, c1, c2, c3, c4) \
	{ *(p)++ = (c1); *(p)++ = (c2); *(p)++ = (c3); *(p)++ = (c4); }

#define write_atom_header(p, size, c1, c2, c3, c4) \
	{										\
	write_dword(p, size);					\
	write_atom_name(p, c1, c2, c3, c4);		\
	}

#define write_atom_header64(p, size, c1, c2, c3, c4) \
	{										\
	write_dword(p, 1);						\
	write_atom_name(p, c1, c2, c3, c4);		\
	write_qword(p, size);					\
	}

// typedefs
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char sample_count[4];
	u_char data_offset[4];
} trun_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char sequence_number[4];
} mfhd_atom_t;

typedef struct {
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;
	uint32_t frames_file_index;

	read_cache_state_t* read_cache_state;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t* cur_frame_offset;
	uint32_t cur_frame_pos;
	bool_t first_time;
} fragment_writer_state_t;

// functions
u_char* mp4_builder_write_mfhd_atom(u_char* p, uint32_t segment_index);

size_t mp4_builder_get_trun_atom_size(uint32_t media_type, uint32_t frame_count);

u_char* mp4_builder_write_trun_atom(
	u_char* p, 
	uint32_t media_type, 
	input_frame_t* frames, 
	uint32_t frame_count, 
	uint32_t first_frame_offset);

vod_status_t mp4_builder_frame_writer_init(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	read_cache_state_t* read_cache_state,
	write_callback_t write_callback,
	void* write_context,
	fragment_writer_state_t** result);

vod_status_t mp4_builder_frame_writer_process(fragment_writer_state_t* state);

#endif // __MP4_BUILDER_H__
