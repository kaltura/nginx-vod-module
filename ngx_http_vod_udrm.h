#ifndef _NGX_HTTP_VOD_UDRM_H_INCLUDED_
#define _NGX_HTTP_VOD_UDRM_H_INCLUDED_

// includes
#include "vod/mp4/mp4_encrypt.h"

// functions
ngx_int_t
ngx_http_vod_udrm_parse_response(
	request_context_t* request_context,
	ngx_str_t* drm_info, 
	void** output);

#endif // _NGX_HTTP_VOD_UDRM_H_INCLUDED_
