#ifndef _NGX_HTTP_VOD_HDS_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_HDS_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "vod/hds/hds_manifest.h"
#include "vod/hds/hds_fragment.h"

// typedefs
typedef struct
{
	ngx_flag_t absolute_manifest_urls;
	ngx_str_t manifest_file_name_prefix;
	hds_manifest_config_t manifest_config;
	hds_fragment_config_t fragment_config;
} ngx_http_vod_hds_loc_conf_t;

#endif // _NGX_HTTP_VOD_HDS_CONF_H_INCLUDED_
