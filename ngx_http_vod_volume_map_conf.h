#ifndef _NGX_HTTP_VOD_VOLUME_MAP_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_VOLUME_MAP_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef struct
{
	ngx_str_t file_name_prefix;
	uintptr_t interval;
} ngx_http_vod_volume_map_loc_conf_t;

#endif // _NGX_HTTP_VOD_VOLUME_MAP_CONF_H_INCLUDED_