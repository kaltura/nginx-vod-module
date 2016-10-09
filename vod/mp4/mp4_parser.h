#ifndef __MP4_PARSER_H__
#define __MP4_PARSER_H__

// includes
#include "mp4_parser_base.h"

// functions
vod_status_t mp4_parser_get_ftyp_atom_into(
	request_context_t* request_context,
	const u_char* buffer,
	size_t buffer_size,
	const u_char** ptr,
	size_t* size);

vod_status_t mp4_parser_get_moov_atom_info(
	request_context_t* request_context, 
	const u_char* buffer, 
	size_t buffer_size, 
	off_t* offset, 
	size_t* size);

vod_status_t mp4_parser_uncompress_moov(
	request_context_t* request_context,
	const u_char* buffer,
	size_t size,
	size_t max_moov_size,
	u_char** out_buffer,
	off_t* moov_offset,
	size_t* moov_size);

vod_status_t mp4_parser_parse_basic_metadata(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	media_base_metadata_t** result);

vod_status_t mp4_parser_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result);

#endif // __MP4_PARSER_H__
