#ifndef _NGX_HTTP_VOD_MODULE_H_INCLUDED_
#define _NGX_HTTP_VOD_MODULE_H_INCLUDED_

// includes
#include <ngx_http.h>

// macros
#define NGINX_VOD_VERSION "1.0"

// globals
extern ngx_module_t  ngx_http_vod_module;

// main
ngx_int_t ngx_http_vod_handler(ngx_http_request_t *r);

// handlers
ngx_int_t ngx_http_vod_local_request_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_vod_mapped_request_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_vod_remote_request_handler(ngx_http_request_t *r);

#endif // _NGX_HTTP_VOD_MODULE_H_INCLUDED_
