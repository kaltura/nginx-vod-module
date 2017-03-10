#ifndef __SUBTITLE_FORMAT_H__
#define __SUBTITLE_FORMAT_H__

// includes
#include "../media_format.h"

// constants
#define WEBVTT_HEADER_NEWLINES ("WEBVTT\r\n\r\n")

// typedefs
typedef struct {
	media_base_metadata_t base;
	vod_str_t source;
} subtitle_base_metadata_t;

// functions
vod_status_t subtitle_reader_init(
	request_context_t* request_context,
	size_t initial_read_size,
	void** ctx);

vod_status_t subtitle_reader_read(
	void* ctx,
	uint64_t offset,
	vod_str_t* buffer,
	media_format_read_metadata_result_t* result);

vod_status_t subtitle_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	uint64_t full_duration,
	size_t metadata_part_count,
	media_base_metadata_t** result);

#endif //__SUBTITLE_FORMAT_H__
