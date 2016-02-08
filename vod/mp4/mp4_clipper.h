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
	media_clipper_parse_result_t base;
	mvhd_clip_result_t mvhd;
	vod_array_t parsed_traks;
	bool_t copy_data;
	size_t alloc_size;
	size_t moov_atom_size;
} mp4_clipper_parse_result_t;

// functions
vod_status_t mp4_clipper_parse_moov(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	bool_t copy_data,
	media_clipper_parse_result_t** result);

vod_status_t mp4_clipper_build_header(
	request_context_t* request_context,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	media_clipper_parse_result_t* parse_result,
	vod_chain_t** result,
	size_t* response_size,
	vod_str_t* content_type);

#endif // __MP4_CLIPPER_H__
