#ifndef _NGX_HTTP_VOD_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "ngx_http_vod_dash_conf.h"
#include "ngx_http_vod_hds_conf.h"
#include "ngx_http_vod_hls_conf.h"
#include "ngx_http_vod_mss_conf.h"

// typedefs
struct ngx_http_vod_request_params_s;

struct ngx_http_vod_loc_conf_s {
	// config fields
	ngx_str_t child_request_location;
	int (*get_file_path_components)(ngx_str_t* uri);
	ngx_int_t (*parse_uri_file_name)(
		ngx_http_request_t *r,
		struct ngx_http_vod_loc_conf_s *conf,
		u_char* start_pos,
		u_char* end_pos,
		struct ngx_http_vod_request_params_s* request_params);
	ngx_int_t(*request_handler)(ngx_http_request_t *r);
	ngx_str_t multi_uri_suffix;
	ngx_uint_t segment_duration;
	ngx_str_t secret_key;
	ngx_uint_t duplicate_bitrate_threshold;
	ngx_str_t https_header_name;
	ngx_shm_zone_t* moov_cache_zone;
	size_t initial_read_size;
	size_t max_moov_size;
	size_t cache_buffer_size;
	ngx_http_upstream_conf_t upstream;
	ngx_str_t upstream_host_header;
	ngx_str_t upstream_extra_args;
	ngx_shm_zone_t* path_mapping_cache_zone;
	ngx_str_t path_response_prefix;
	ngx_str_t path_response_postfix;
	size_t max_path_length;
	ngx_http_upstream_conf_t   fallback_upstream;
	ngx_str_t proxy_header_name;
	ngx_str_t proxy_header_value;

	ngx_str_t clip_to_param_name;
	ngx_str_t clip_from_param_name;
	ngx_str_t tracks_param_name;

	// derived fields
	ngx_str_t proxy_header;
	ngx_hash_t uri_params_hash;

	// submodules
	ngx_http_vod_dash_loc_conf_t dash;
	ngx_http_vod_hds_loc_conf_t hds;
	ngx_http_vod_hls_loc_conf_t hls;
	ngx_http_vod_mss_loc_conf_t mss;
};

typedef struct ngx_http_vod_loc_conf_s ngx_http_vod_loc_conf_t;

// globals
extern ngx_http_module_t ngx_http_vod_module_ctx;
extern ngx_command_t ngx_http_vod_commands[];

#endif // _NGX_HTTP_VOD_CONF_H_INCLUDED_
