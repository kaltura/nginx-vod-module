#ifndef _NGX_HTTP_VOD_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_dash_conf.h"
#include "ngx_http_vod_hds_conf.h"
#include "ngx_http_vod_hls_conf.h"
#include "ngx_http_vod_mss_conf.h"
#include "vod/segmenter.h"

// enum
enum {
	CACHE_TYPE_VOD,
	CACHE_TYPE_LIVE,

	CACHE_TYPE_COUNT
};

// typedefs
struct ngx_http_vod_request_params_s;

struct ngx_http_vod_loc_conf_s {
	// config fields
	ngx_http_vod_submodule_t submodule;
	ngx_str_t upstream_location;
	ngx_int_t(*request_handler)(ngx_http_request_t *r);
	ngx_str_t multi_uri_suffix;
	segmenter_conf_t segmenter;
	ngx_http_complex_value_t *secret_key;
	ngx_str_t https_header_name;
	ngx_str_t segments_base_url;
	ngx_flag_t segments_base_url_has_scheme;
	ngx_buffer_cache_t* metadata_cache;
	ngx_buffer_cache_t* response_cache[CACHE_TYPE_COUNT];
	size_t initial_read_size;
	size_t max_metadata_size;
	size_t max_frames_size;
	size_t cache_buffer_size;
	buffer_pool_t* output_buffer_pool;
	size_t max_upstream_headers_size;
	ngx_flag_t ignore_edit_list;
	ngx_http_complex_value_t *upstream_extra_args;
	ngx_buffer_cache_t* path_mapping_cache[CACHE_TYPE_COUNT];
	ngx_str_t path_response_prefix;
	ngx_str_t path_response_postfix;
	size_t max_mapping_response_size;
	ngx_str_t fallback_upstream_location;
	ngx_table_elt_t proxy_header;

	time_t last_modified_time;
	ngx_hash_t  last_modified_types;
	ngx_array_t *last_modified_types_keys;

	ngx_flag_t drm_enabled;
	ngx_uint_t drm_clear_lead_segment_count;
	ngx_str_t drm_upstream_location;
	size_t drm_max_info_length;
	ngx_buffer_cache_t* drm_info_cache;
	ngx_http_complex_value_t *drm_request_uri;

	ngx_str_t clip_to_param_name;
	ngx_str_t clip_from_param_name;
	ngx_str_t tracks_param_name;
	ngx_str_t speed_param_name;

	ngx_shm_zone_t* perf_counters_zone;

#if (NGX_THREADS)
	ngx_thread_pool_t *open_file_thread_pool;
#endif

	// derived fields
	ngx_hash_t uri_params_hash;
	ngx_hash_t pd_uri_params_hash;

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
