#ifndef _NGX_FILE_READER_H_INCLUDED_
#define _NGX_FILE_READER_H_INCLUDED_

// includes
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>

#if (NGX_THREADS)
#include "ngx_async_open_file_cache.h"
#endif

// typedefs
typedef void (*ngx_async_read_callback_t)(void* context, ngx_int_t rc, ssize_t bytes_read);

typedef struct {
	ngx_http_request_t *r;
	ngx_file_t file;
	off_t directio;
	ngx_flag_t log_not_found;
	ngx_log_t* log;
	off_t file_size;
#if (NGX_HAVE_FILE_AIO)
	ngx_flag_t use_aio;
	ngx_async_read_callback_t read_callback;
	void* callback_context;
#endif
} ngx_file_reader_state_t;

// functions
ngx_int_t ngx_file_reader_init(
	ngx_file_reader_state_t* state,
	ngx_async_read_callback_t read_callback,
	void* callback_context,
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t  *clcf,
	ngx_str_t* path);

#if (NGX_THREADS)
ngx_int_t ngx_file_reader_init_async(
	ngx_file_reader_state_t* state,
	void** context,
	ngx_thread_pool_t *thread_pool,
	ngx_async_open_file_callback_t open_callback,
	ngx_async_read_callback_t read_callback,
	void* callback_context,
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t  *clcf,
	ngx_str_t* path);
#endif

ngx_int_t ngx_file_reader_dump_file_part(ngx_file_reader_state_t* state, off_t start, off_t end);

ssize_t ngx_async_file_read(ngx_file_reader_state_t* state, u_char *buf, size_t size, off_t offset);

ngx_int_t ngx_file_reader_enable_directio(ngx_file_reader_state_t* state);

#endif // _NGX_FILE_READER_H_INCLUDED_
