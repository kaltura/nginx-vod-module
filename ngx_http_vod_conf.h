#ifndef _NGX_HTTP_VOD_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "vod/m3u8_builder.h"

// typedefs
typedef struct ngx_http_vod_loc_conf_s {
    struct ngx_http_vod_loc_conf_s *parent;

	// config fields
	ngx_http_upstream_conf_t   upstream;
	ngx_http_upstream_conf_t   fallback_upstream;
	ngx_str_t proxy_header_name;
	ngx_str_t proxy_header_value;
	ngx_int_t(*request_handler)(ngx_http_request_t *r);
	ngx_str_t secret_key;
	ngx_str_t upstream_host_header;
	ngx_str_t upstream_extra_args;
	ngx_str_t path_response_prefix;
	ngx_str_t path_response_postfix;
	ngx_uint_t segment_duration;
	size_t initial_read_size;
	size_t max_moov_size;
	size_t max_path_length;
	size_t cache_buffer_size;
	ngx_str_t clip_to_param_name;
	ngx_str_t clip_from_param_name;
	ngx_str_t encryption_key_file_name;
	ngx_str_t index_file_name_prefix;
	ngx_str_t iframes_file_name_prefix;

	// derived fields
	m3u8_config_t m3u8_config;
	ngx_str_t proxy_header;
} ngx_http_vod_loc_conf_t;

// globals
extern ngx_http_module_t ngx_http_vod_module_ctx;
extern ngx_command_t ngx_http_vod_commands[];

#endif // _NGX_HTTP_VOD_CONF_H_INCLUDED_
