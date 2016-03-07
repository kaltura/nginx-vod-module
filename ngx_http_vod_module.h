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

// variables
ngx_int_t ngx_http_vod_set_filepath_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_vod_set_suburi_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_vod_set_sequence_id_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_vod_set_clip_id_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_vod_set_dynamic_mapping_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_vod_set_request_params_var(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);

// handlers
ngx_int_t ngx_http_vod_local_request_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_vod_mapped_request_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_vod_remote_request_handler(ngx_http_request_t *r);

#endif // _NGX_HTTP_VOD_MODULE_H_INCLUDED_
