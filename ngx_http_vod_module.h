#ifndef _NGX_HTTP_VOD_MODULE_H_INCLUDED_
#define _NGX_HTTP_VOD_MODULE_H_INCLUDED_

// includes
#include <ngx_http.h>

// globals
extern ngx_module_t  ngx_http_vod_module;

// main
ngx_int_t ngx_http_vod_handler(ngx_http_request_t *r);

// handlers
ngx_int_t local_request_handler(ngx_http_request_t *r);
ngx_int_t mapped_request_handler(ngx_http_request_t *r);
ngx_int_t remote_request_handler(ngx_http_request_t *r);

#endif // _NGX_HTTP_VOD_MODULE_H_INCLUDED_
