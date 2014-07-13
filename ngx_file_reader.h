#ifndef _NGX_FILE_READER_H_INCLUDED_
#define _NGX_FILE_READER_H_INCLUDED_

// includes
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>

// typedefs
typedef void (*async_read_callback_t)(void* context, ngx_int_t rc, ssize_t bytes_read);

typedef struct {
	ngx_http_request_t *r;
	ngx_file_t file;
	ngx_flag_t use_directio;
	ngx_log_t* log;
#if (NGX_HAVE_FILE_AIO)
	ngx_flag_t use_aio;
	async_read_callback_t callback;
	void* callback_context;
#endif
} file_reader_state_t;

// functions
ngx_int_t file_reader_init(
	file_reader_state_t* state,
	async_read_callback_t callback,
	void* callback_context,
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t  *clcf,
	ngx_str_t* path);

ssize_t async_file_read(file_reader_state_t* state, u_char *buf, size_t size, off_t offset);

ngx_int_t file_reader_enable_directio(file_reader_state_t* state);

#endif // _NGX_FILE_READER_H_INCLUDED_
