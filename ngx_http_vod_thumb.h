#ifndef _NGX_HTTP_VOD_THUMB_H_INCLUDED_
#define _NGX_HTTP_VOD_THUMB_H_INCLUDED_

// includes
#include "ngx_http_vod_submodule.h"

// globals
extern const ngx_http_vod_submodule_t thumb;

// functions
ngx_int_t ngx_http_vod_thumb_get_url(
	ngx_http_vod_submodule_context_t* submodule_context,
	uint32_t sequences_mask,
	ngx_str_t* result);

#endif // _NGX_HTTP_VOD_THUMB_H_INCLUDED_
