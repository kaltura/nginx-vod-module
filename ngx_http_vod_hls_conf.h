#ifndef _NGX_HTTP_VOD_HLS_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_HLS_CONF_H_INCLUDED_

// includes
#include "vod/hls/m3u8_builder.h"
#include "vod/hls/hls_muxer.h"

// typedefs
typedef struct
{
	ngx_flag_t absolute_master_urls;
	ngx_flag_t absolute_index_urls;
	ngx_flag_t absolute_iframe_urls;
	ngx_str_t master_file_name_prefix;
	bool_t interleave_frames;
	bool_t align_frames;
	bool_t align_pts;
	bool_t output_id3_timestamps;
	ngx_http_complex_value_t* id3_data;
	vod_uint_t encryption_method;
	ngx_http_complex_value_t* encryption_key_uri;
	bool_t output_iv;

	// derived fields
	m3u8_config_t m3u8_config;
} ngx_http_vod_hls_loc_conf_t;

// globals
extern ngx_conf_enum_t  hls_encryption_methods[];
extern ngx_conf_enum_t  hls_container_formats[];

#endif // _NGX_HTTP_VOD_HLS_CONF_H_INCLUDED_
