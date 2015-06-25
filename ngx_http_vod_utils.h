#ifndef _NGX_HTTP_VOD_UTILS_H_INCLUDED_
#define _NGX_HTTP_VOD_UTILS_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "ngx_http_vod_request_parse.h"
#include "ngx_http_vod_conf.h"
#include "vod/common.h"

// functions
ngx_int_t ngx_http_vod_send_response(ngx_http_request_t *r, ngx_str_t *response, ngx_str_t* content_type);

ngx_int_t ngx_http_vod_status_to_ngx_error(vod_status_t rc);

ngx_flag_t ngx_http_vod_header_exists(ngx_http_request_t* r, ngx_str_t* searched_header);

void ngx_http_vod_get_base_url(
	ngx_http_request_t* r,
	ngx_str_t* https_header_name,
	ngx_str_t* conf_base_url,
	ngx_flag_t conf_base_url_has_schema,
	ngx_str_t* file_uri,
	ngx_str_t* base_url);

ngx_int_t ngx_http_vod_merge_string_parts(
	ngx_http_request_t* r,
	ngx_str_t* parts,
	uint32_t part_count,
	ngx_str_t* result);

ngx_int_t ngx_http_vod_range_parse(
	ngx_str_t* range, 
	off_t content_length, 
	off_t* out_start, 
	off_t* out_end);

#endif // _NGX_HTTP_VOD_UTILS_H_INCLUDED_
