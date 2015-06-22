#ifndef _NGX_CHILD_HTTP_REQUEST_INCLUDED_
#define _NGX_CHILD_HTTP_REQUEST_INCLUDED_

// includes
#include <ngx_http.h>

#define DEFINE_UPSTREAM_COMMANDS(member, command_prefix)							\
	{ ngx_string("vod_" command_prefix "upstream"),									\
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,	\
	ngx_http_upstream_command,														\
	NGX_HTTP_LOC_CONF_OFFSET,														\
	offsetof(ngx_http_vod_loc_conf_t, member),										\
	NULL },																			\
																					\
	{ ngx_string("vod_" command_prefix "connect_timeout"),							\
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,	\
	ngx_conf_set_msec_slot,															\
	NGX_HTTP_LOC_CONF_OFFSET,														\
	offsetof(ngx_http_vod_loc_conf_t, member.connect_timeout),						\
	NULL },																			\
																					\
	{ ngx_string("vod_" command_prefix "send_timeout"),								\
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,	\
	ngx_conf_set_msec_slot,															\
	NGX_HTTP_LOC_CONF_OFFSET,														\
	offsetof(ngx_http_vod_loc_conf_t, member.send_timeout),							\
	NULL },																			\
																					\
	{ ngx_string("vod_" command_prefix "read_timeout"),								\
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,	\
	ngx_conf_set_msec_slot,															\
	NGX_HTTP_LOC_CONF_OFFSET,														\
	offsetof(ngx_http_vod_loc_conf_t, member.read_timeout),							\
	NULL },




// typedefs
typedef void (*ngx_child_request_callback_t)(void* context, ngx_int_t rc, off_t content_length, ngx_buf_t* response);

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
	ngx_flag_t escape_uri;
} ngx_child_request_params_t;

// Note: the purpose of this struct is to reuse the request & headers buffers between several 
//		child requests under the same parent request. it is significant only in remote mode.
typedef struct {
	ngx_buf_t* request_buffer;
	u_char* headers_buffer;
	size_t headers_buffer_size;
} ngx_child_request_buffers_t;

// request initiation function

// IMPORTANT NOTE: due to the implementation of ngx_http_upstream_process_body_in_memory, and in order to
//		avoid memory copy operations, this function assumes that the buffer passed to it has a size of 
//		at least max_response_length + 1 (unlike max_response_length like one would expect)
//		this issue is discussed in http://trac.nginx.org/nginx/ticket/680

ngx_int_t ngx_child_request_start(
	ngx_http_request_t *r,
	ngx_child_request_buffers_t* buffers,
	ngx_child_request_callback_t callback,
	void* callback_context,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* internal_location,
	ngx_child_request_params_t* params,
	off_t max_response_length,
	u_char* response_buffer);

ngx_int_t ngx_dump_request(
	ngx_http_request_t *r,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_child_request_params_t* params);

// free function
void ngx_child_request_free_buffers(ngx_pool_t* pool, ngx_child_request_buffers_t* buffers);

// config functions
void ngx_init_upstream_conf(ngx_http_upstream_conf_t* upstream);

char *ngx_merge_upstream_conf(
	ngx_conf_t *cf,
	ngx_http_upstream_conf_t* conf_upstream,
	ngx_http_upstream_conf_t* prev_upstream);

char *ngx_http_upstream_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

ngx_int_t ngx_child_request_internal_handler(ngx_http_request_t *r);

#endif // _NGX_CHILD_HTTP_REQUEST_INCLUDED_
