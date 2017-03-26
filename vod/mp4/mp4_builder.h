#ifndef __MP4_BUILDER_H__
#define __MP4_BUILDER_H__

// includes
#include "../write_stream.h"
#include "../media_set.h"

// macros
#define write_atom_name(p, c1, c2, c3, c4) \
	{ *(p)++ = (c1); *(p)++ = (c2); *(p)++ = (c3); *(p)++ = (c4); }

#define write_atom_header(p, size, c1, c2, c3, c4) \
	{										\
	write_be32(p, size);					\
	write_atom_name(p, c1, c2, c3, c4);		\
	}

#define write_atom_header64(p, size, c1, c2, c3, c4) \
	{										\
	write_be32(p, 1);						\
	write_atom_name(p, c1, c2, c3, c4);		\
	write_be64(p, size);					\
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
	bool_t reuse_buffers;

	media_sequence_t* sequence;
	media_clip_filtered_t* cur_clip;
	frame_list_part_t* first_frame_part;
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	bool_t first_time;
	bool_t frame_started;
} fragment_writer_state_t;

// functions
u_char* mp4_builder_write_mfhd_atom(u_char* p, uint32_t segment_index);

size_t mp4_builder_get_trun_atom_size(uint32_t media_type, uint32_t frame_count);

u_char* mp4_builder_write_trun_atom(
	u_char* p, 
	media_sequence_t* sequence, 
	uint32_t first_frame_offset,
	uint32_t version);

vod_status_t mp4_builder_frame_writer_init(
	request_context_t* request_context,
	media_sequence_t* sequence,
	write_callback_t write_callback,
	void* write_context,
	bool_t reuse_buffers,
	fragment_writer_state_t** result);

vod_status_t mp4_builder_frame_writer_process(fragment_writer_state_t* state);

#endif // __MP4_BUILDER_H__
