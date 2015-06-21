#ifndef __MP4_CLIPPER_H__
#define __MP4_CLIPPER_H__

// includes
#include "mp4_parser_base.h"

// typedefs
typedef struct {
	atom_info_t atom;
	uint64_t duration;
	u_char version;
} mvhd_clip_result_t;

typedef struct {
	mvhd_clip_result_t mvhd;
	vod_array_t parsed_traks;
	bool_t copy_data;
	size_t alloc_size;
	size_t moov_atom_size;
	uint64_t min_first_offset;
	uint64_t max_last_offset;
} mp4_clipper_parse_result_t;

// functions
vod_status_t mp4_clipper_parse_moov(
	request_context_t* request_context,
	mpeg_parse_params_t* parse_params,
	bool_t copy_data,
	u_char* buffer,
	size_t size,
	mp4_clipper_parse_result_t* result);

vod_status_t mp4_clipper_build_header(
	request_context_t* request_context,
	u_char* ftyp_buffer,
	size_t ftyp_size,
	mp4_clipper_parse_result_t* parse_result,
	vod_chain_t** result,
	size_t* response_size);

#endif // __MP4_CLIPPER_H__
