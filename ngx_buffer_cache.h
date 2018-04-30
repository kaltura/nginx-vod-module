#ifndef _NGX_BUFFER_CACHE_H_INCLUDED_
#define _NGX_BUFFER_CACHE_H_INCLUDED_

// includes
#include <ngx_core.h>

// constants
#define BUFFER_CACHE_KEY_SIZE (16)

// typedefs
struct ngx_buffer_cache_s;
typedef struct ngx_buffer_cache_s ngx_buffer_cache_t;

typedef struct {
	ngx_atomic_t store_ok;
	ngx_atomic_t store_bytes;
	ngx_atomic_t store_err;
	ngx_atomic_t store_exists;
	ngx_atomic_t fetch_hit;
	ngx_atomic_t fetch_bytes;
	ngx_atomic_t fetch_miss;
	ngx_atomic_t evicted;
	ngx_atomic_t evicted_bytes;
	ngx_atomic_t reset;

	// updated only when the stats are fetched
	ngx_atomic_t entries;
	ngx_atomic_t data_size;
} ngx_buffer_cache_stats_t;

// functions
ngx_flag_t ngx_buffer_cache_fetch(
	ngx_buffer_cache_t* cache,
	u_char* key,
	ngx_str_t* buffer,
	uint32_t* token);

void ngx_buffer_cache_release(
	ngx_buffer_cache_t* cache,
	u_char* key,
	uint32_t token);

ngx_flag_t ngx_buffer_cache_store(
	ngx_buffer_cache_t* cache,
	u_char* key,
	u_char* source_buffer,
	size_t buffer_size);

ngx_flag_t ngx_buffer_cache_store_gather(
	ngx_buffer_cache_t* cache,
	u_char* key,
	ngx_str_t* buffers,
	size_t buffer_count);

void ngx_buffer_cache_get_stats(
	ngx_buffer_cache_t* cache,
	ngx_buffer_cache_stats_t* stats);

void ngx_buffer_cache_reset_stats(ngx_buffer_cache_t* cache);

ngx_buffer_cache_t* ngx_buffer_cache_create(
	ngx_conf_t *cf, 
	ngx_str_t *name, 
	size_t size, 
	time_t expiration, 
	void *tag);

#endif // _NGX_BUFFER_CACHE_H_INCLUDED_
