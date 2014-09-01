#ifndef _NGX_HTTP_VOD_UTILS_H_INCLUDED_
#define _NGX_HTTP_VOD_UTILS_H_INCLUDED_

// includes
#include <ngx_http.h>
#include "vod/common.h"

// functions
ngx_int_t ngx_http_vod_send_response(ngx_http_request_t *r, ngx_str_t *response, u_char* content_type, size_t content_type_len);

ngx_int_t ngx_http_vod_status_to_ngx_error(vod_status_t rc);

ngx_flag_t ngx_http_vod_header_exists(ngx_http_request_t* r, ngx_str_t* searched_header);

#endif // _NGX_HTTP_VOD_UTILS_H_INCLUDED_
