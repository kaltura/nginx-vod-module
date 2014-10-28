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
	dash_manifest_config_t mpd_config;
} ngx_http_vod_dash_loc_conf_t;

#endif // _NGX_HTTP_VOD_DASH_CONF_H_INCLUDED_
