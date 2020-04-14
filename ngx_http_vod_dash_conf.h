#ifndef _NGX_HTTP_VOD_DASH_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_DASH_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "vod/dash/dash_packager.h"

// typedefs
typedef struct
{
	ngx_str_t manifest_file_name_prefix;
	ngx_flag_t absolute_manifest_urls;
	ngx_flag_t init_mp4_pssh;
	dash_manifest_config_t mpd_config;
} ngx_http_vod_dash_loc_conf_t;

// globals
extern ngx_conf_enum_t dash_manifest_formats[];

extern ngx_conf_enum_t dash_subtitle_formats[];

#endif // _NGX_HTTP_VOD_DASH_CONF_H_INCLUDED_
