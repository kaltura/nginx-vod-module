#ifndef _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_
#define _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "ngx_buffer_cache.h"
#include "vod/mp4/mp4_parser.h"
#include "vod/media_set.h"

// constants
#define MAX_SUB_URIS (32)
#define MAX_URI_PARAM_NAME_LEN (32)			// clipTo, clipFrom etc.

#define PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX	(0x1)
#define PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE	(0x2)
#define PARSE_FILE_NAME_ALLOW_CLIP_INDEX		(0x4)

// macros
#define ngx_http_vod_starts_with(start_pos, end_pos, prefix)	\
	((end_pos) - (start_pos) >= (int)(prefix)->len && ngx_memcmp((start_pos), (prefix)->data, (prefix)->len) == 0)

#define ngx_http_vod_ends_with(start_pos, end_pos, postfix)	\
	((end_pos) - (start_pos) >= (int)(postfix)->len && ngx_memcmp((end_pos) - (postfix)->len, (postfix)->data, (postfix)->len) == 0)

#define ngx_http_vod_ends_with_static(start_pos, end_pos, postfix)	\
	((end_pos) - (start_pos) >= (int)sizeof(postfix) - 1 && ngx_memcmp((end_pos) - (sizeof(postfix) - 1), (postfix), sizeof(postfix) - 1) == 0)

#define ngx_http_vod_match_prefix_postfix(start_pos, end_pos, prefix, postfix)				\
	((end_pos) - (start_pos) >= (int)(prefix)->len + (int)sizeof(postfix) - 1 &&			\
	ngx_memcmp((end_pos) - (sizeof(postfix) - 1), (postfix), sizeof(postfix) - 1) == 0 &&	\
	ngx_memcmp((start_pos), (prefix)->data, (prefix)->len) == 0)

// typedefs
struct ngx_http_vod_request_s;
struct ngx_http_vod_loc_conf_s;

enum {
	MATCH_END,
	MATCH_FIXED_STRING,
	MATCH_DELIM_STRING,
	MATCH_NUMBER,
};

typedef struct {
	int match_type;
	int target_offset;
	int delim;
	ngx_str_t string;
} ngx_http_vod_match_definition_t;

// functions
bool_t ngx_http_vod_split_uri_file_name(
	ngx_str_t* uri,
	int components,
	ngx_str_t* path,
	ngx_str_t* file_name);

ngx_int_t ngx_http_vod_parse_uri_path(
	ngx_http_request_t* r,
	ngx_str_t* multi_uri_suffix,
	ngx_hash_t* params_hash,
	ngx_str_t* uri,
	request_params_t* request_params,
	media_set_t* media_set);

ngx_int_t ngx_http_vod_init_uri_params_hash(
	ngx_conf_t *cf, 
	struct ngx_http_vod_loc_conf_s* conf);

// utility functions for submodules
u_char* ngx_http_vod_extract_uint32_token_reverse(
	u_char* start_pos, 
	u_char* end_pos, 
	uint32_t* result);

bool_t ngx_http_vod_parse_string(
	const ngx_http_vod_match_definition_t* match_def,
	u_char* start_pos,
	u_char* end_pos,
	void* output);

ngx_int_t ngx_http_vod_parse_uri_file_name(
	ngx_http_request_t* r,
	u_char* start_pos,
	u_char* end_pos,
	uint32_t flags,
	request_params_t* result);

#endif // _NGX_HTTP_VOD_REQUEST_PARSE_H_INCLUDED_
