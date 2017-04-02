#ifndef _NGX_HTTP_VOD_MODULE_H_INCLUDED_
#define _NGX_HTTP_VOD_MODULE_H_INCLUDED_

// includes
#include <ngx_http.h>
#include <ngx_vod_version.h>

// globals
extern ngx_module_t  ngx_http_vod_module;

// main
ngx_int_t ngx_http_vod_handler(ngx_http_request_t *r);

// variables
ngx_int_t ngx_http_vod_preconfiguration(ngx_conf_t *cf);

// handlers
ngx_int_t ngx_http_vod_local_request_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_vod_mapped_request_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_vod_remote_request_handler(ngx_http_request_t *r);

#endif // _NGX_HTTP_VOD_MODULE_H_INCLUDED_
