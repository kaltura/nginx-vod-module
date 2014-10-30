#ifndef _NGX_HTTP_VOD_HLS_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_HLS_CONF_H_INCLUDED_

// includes
#include "vod/hls/m3u8_builder.h"

// typedefs
typedef struct
{
	ngx_flag_t absolute_master_urls;
	ngx_flag_t absolute_index_urls;
	ngx_flag_t absolute_iframe_urls;
	ngx_str_t master_file_name_prefix;
	ngx_str_t iframes_file_name_prefix;

	// derived fields
	m3u8_config_t m3u8_config;
} ngx_http_vod_hls_loc_conf_t;

#endif // _NGX_HTTP_VOD_HLS_CONF_H_INCLUDED_
