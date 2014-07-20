#ifndef _NGX_CHILD_HTTP_REQUEST_INCLUDED_
#define _NGX_CHILD_HTTP_REQUEST_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef void (*child_request_callback_t)(void* context, ngx_int_t rc, ngx_buf_t* response);

typedef struct {
	child_request_callback_t callback;
	void* callback_context;
	ngx_event_t* complete_event;
	ngx_int_t request_status;

	ngx_buf_t* request_buffer;

	u_char* headers_buffer;
	size_t headers_buffer_size;
	ngx_flag_t save_response_headers;

	u_char* response_buffer;
	off_t response_buffer_size;

	ngx_pool_cleanup_t *cleanup;
} child_request_context_t;

// request initiation function
ngx_int_t child_request_start(
	ngx_http_request_t *r,
	child_request_callback_t callback,
	void* callback_context,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* base_uri,
	ngx_str_t* extra_args,
	ngx_str_t* host_name,
	off_t range_start,
	off_t range_end,
	off_t max_response_length,
	u_char* response_buffer);

ngx_int_t dump_request(
	ngx_http_request_t *r,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* uri,
	ngx_str_t* host_name,
	ngx_str_t* extra_headers);

// free function
void child_request_free(ngx_http_request_t *r);

// config functions
void init_upstream_conf(ngx_http_upstream_conf_t* upstream);

char *merge_upstream_conf(
	ngx_conf_t *cf,
	ngx_http_upstream_conf_t* conf_upstream,
	ngx_http_upstream_conf_t* prev_upstream);

#endif // _NGX_CHILD_HTTP_REQUEST_INCLUDED_
