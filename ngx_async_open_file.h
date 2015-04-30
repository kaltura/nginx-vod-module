#ifndef _NGX_ASYNC_OPEN_FILE_H_INCLUDED_
#define _NGX_ASYNC_OPEN_FILE_H_INCLUDED_

// includes
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_thread_pool.h>

// typedefs
typedef void(*ngx_async_open_file_callback_t)(void* context, ngx_int_t rc);

// functions
ngx_int_t
ngx_async_open_file(
	ngx_pool_t* pool,
	ngx_thread_pool_t *tp,
	ngx_thread_task_t **taskp,
	ngx_str_t *name,
	ngx_open_file_info_t *of,
	ngx_async_open_file_callback_t callback,
	void* context);

#endif // _NGX_ASYNC_OPEN_FILE_H_INCLUDED_
