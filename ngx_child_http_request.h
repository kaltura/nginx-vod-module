#ifndef _NGX_CHILD_HTTP_REQUEST_INCLUDED_
#define _NGX_CHILD_HTTP_REQUEST_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef void(*ngx_child_request_callback_t)(void* context, ngx_int_t rc, ngx_buf_t* buf, ssize_t bytes_read);

typedef struct {
	ngx_uint_t method;
	ngx_str_t base_uri;
	ngx_str_t extra_args;
	off_t range_start;
	off_t range_end;
	ngx_table_elt_t extra_header;
	ngx_flag_t proxy_range;
	ngx_flag_t proxy_all_headers;
} ngx_child_request_params_t;

// functions

// Notes:
//	1. callback is optional, if it is not supplied, the module will finalize the request
//		when the upstream request completes.
//	2. response_buffer is optional, if it is not supplied, the upstream response gets written
//		to the parent request. when a response buffer is supplied, the response is written to it, 
//		the buffer should be large enough to contain both the response body and the response headers.
ngx_int_t ngx_child_request_start(
	ngx_http_request_t *r,
	ngx_child_request_callback_t callback,
	void* callback_context,
	ngx_str_t* internal_location,
	ngx_child_request_params_t* params,
	ngx_buf_t* response_buffer);

ngx_int_t ngx_child_request_init(ngx_conf_t *cf);

#endif // _NGX_CHILD_HTTP_REQUEST_INCLUDED_
