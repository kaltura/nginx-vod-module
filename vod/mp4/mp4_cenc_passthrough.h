#ifndef __MP4_CENC_PASSTHROUGH_H__
#define __MP4_CENC_PASSTHROUGH_H__

// includes
#include "../media_set.h"

// typedefs
typedef struct {
	media_sequence_t* sequence;
	uint8_t default_auxiliary_sample_size;
	bool_t use_subsamples;
	size_t saiz_atom_size;
	size_t saio_atom_size;
	size_t auxiliary_info_size;
	size_t total_size;
} mp4_cenc_passthrough_context_t;

// functions
bool_t mp4_cenc_passthrough_init(
	mp4_cenc_passthrough_context_t* context,
	media_sequence_t* sequence);

u_char* mp4_cenc_passthrough_write_saiz_saio(
	mp4_cenc_passthrough_context_t* context,
	u_char* p,
	size_t auxiliary_data_offset);

#endif //__MP4_CENC_PASSTHROUGH_H__
