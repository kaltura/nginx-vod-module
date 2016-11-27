#ifndef _NGX_HTTP_VOD_THUMB_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_THUMB_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef struct
{
	ngx_str_t file_name_prefix;
	ngx_flag_t accurate;
} ngx_http_vod_thumb_loc_conf_t;

#endif // _NGX_HTTP_VOD_THUMB_CONF_H_INCLUDED_
