
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_open_file_cache.h>
#include <ngx_thread_pool.h>

#ifndef _NGX_ASYNC_OPEN_FILE_CACHE_H_INCLUDED_
#define _NGX_ASYNC_OPEN_FILE_CACHE_H_INCLUDED_


typedef void(*ngx_async_open_file_callback_t)(void* context, ngx_int_t rc);


ngx_int_t ngx_async_open_cached_file(
	ngx_open_file_cache_t *cache, 
	ngx_str_t *name,
    ngx_open_file_info_t *of, 
	ngx_pool_t *pool, 
	ngx_thread_pool_t *tp, 
	ngx_thread_task_t **taskp, 
	ngx_async_open_file_callback_t callback, 
	void* context);


#endif /* _NGX_ASYNC_OPEN_FILE_CACHE_H_INCLUDED_ */
