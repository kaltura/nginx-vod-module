#ifndef _NGX_CHILD_HTTP_REQUEST_INCLUDED_
#define _NGX_CHILD_HTTP_REQUEST_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef void (*child_request_callback_t)(void* context, ngx_int_t rc, ngx_buf_t* response);

typedef struct {
	ngx_uint_t method;
	ngx_str_t base_uri;
	ngx_str_t extra_args;
	ngx_str_t host_name;
	off_t range_start;
	off_t range_end;
	ngx_str_t extra_headers;
	ngx_flag_t proxy_range;
	ngx_flag_t proxy_accept_encoding;
} child_request_params_t;

// Note: the purpose of this struct is to reuse the request & headers buffers between several 
//		child requests under the same parent request. it is significant only in remote mode.
typedef struct {
	ngx_buf_t* request_buffer;
	u_char* headers_buffer;
	size_t headers_buffer_size;
} child_request_buffers_t;

// request initiation function
ngx_int_t
child_request_start(
	ngx_http_request_t *r,
	child_request_buffers_t* buffers,
	child_request_callback_t callback,
	void* callback_context,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* internal_location,
	child_request_params_t* params,
	off_t max_response_length,
	u_char* response_buffer);

ngx_int_t dump_request(
	ngx_http_request_t *r,
	ngx_http_upstream_conf_t* upstream_conf,
	child_request_params_t* params);

// free function
void child_request_free_buffers(ngx_pool_t* pool, child_request_buffers_t* buffers);

// config functions
void init_upstream_conf(ngx_http_upstream_conf_t* upstream);

char *merge_upstream_conf(
	ngx_conf_t *cf,
	ngx_http_upstream_conf_t* conf_upstream,
	ngx_http_upstream_conf_t* prev_upstream);

ngx_int_t child_request_internal_handler(ngx_http_request_t *r);

#endif // _NGX_CHILD_HTTP_REQUEST_INCLUDED_
