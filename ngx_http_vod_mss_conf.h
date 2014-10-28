#ifndef _NGX_HTTP_VOD_MSS_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_MSS_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef struct
{
	ngx_str_t manifest_file_name_prefix;
} ngx_http_vod_mss_loc_conf_t;

#endif // _NGX_HTTP_VOD_MSS_CONF_H_INCLUDED_
