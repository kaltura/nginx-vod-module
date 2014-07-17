#ifndef _NGX_BUFFER_CACHE_H_INCLUDED_
#define _NGX_BUFFER_CACHE_H_INCLUDED_

// includes
#include <nginx.h>
#include <ngx_cycle.h>

// constants
#define BUFFER_CACHE_KEY_SIZE (16)

// functions
ngx_flag_t
ngx_buffer_cache_fetch(
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	u_char** buffer,
	size_t* buffer_size);

ngx_flag_t
ngx_buffer_cache_store(
	ngx_shm_zone_t *shm_zone,
	u_char* key,
	const u_char* source_buffer,
	size_t buffer_size);

ngx_shm_zone_t*
ngx_buffer_cache_create_zone(ngx_conf_t *cf, ngx_str_t *name, size_t size, void *tag);

#endif // _NGX_BUFFER_CACHE_H_INCLUDED_