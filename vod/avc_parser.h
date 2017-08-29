#ifndef __AVC_PARSER_H__
#define __AVC_PARSER_H__

// includes
#include "common.h"

// typedefs
typedef struct {
	request_context_t* request_context;
	vod_array_t sps;
	vod_array_t pps;
} avc_parse_ctx_t;

// functions
vod_status_t avc_parser_init_ctx(
	avc_parse_ctx_t* ctx, 
	request_context_t* request_context);

vod_status_t avc_parser_parse_extra_data(
	avc_parse_ctx_t* ctx,
	vod_str_t* extra_data);

vod_status_t avc_parser_get_slice_header_size(
	avc_parse_ctx_t* ctx,
	const u_char* buffer,
	uint32_t size,
	uint32_t* result);

#endif // __AVC_PARSER_H__
