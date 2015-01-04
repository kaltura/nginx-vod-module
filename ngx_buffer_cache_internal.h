#ifndef _NGX_BUFFER_CACHE_INTERNAL_H_INCLUDED_
#define _NGX_BUFFER_CACHE_INTERNAL_H_INCLUDED_

#include "ngx_buffer_cache.h"
#include "ngx_queue.h"

// macros
#define container_of(ptr, type, member) (type *)((char *)(ptr) - offsetof(type, member))

// constants
#define CACHE_LOCK_EXPIRATION (5)
#define ENTRY_LOCK_EXPIRATION (5)
#define ENTRIES_ALLOC_MARGIN (1024)		// 1K entries ~= 100KB, we reserve this space to make sure allocating entries does not become the bottleneck
#define BUFFER_ALIGNMENT (16)

// enums
enum {
	CES_FREE,
	CES_ALLOCATED,
	CES_READY,
};

// typedefs
typedef struct {
	ngx_rbtree_node_t node;
	ngx_queue_t queue_node;
	u_char* start_offset;
	size_t buffer_size;
	ngx_atomic_t state;
	time_t access_time;
	u_char key[BUFFER_CACHE_KEY_SIZE];
} ngx_buffer_cache_entry_t;

typedef struct {
	ngx_atomic_t reset;
	time_t access_time;
	ngx_rbtree_t rbtree;
	ngx_rbtree_node_t sentinel;
	ngx_queue_t used_queue;
	ngx_queue_t free_queue;
	ngx_buffer_cache_entry_t* entries_start;
	ngx_buffer_cache_entry_t* entries_end;
	u_char* buffers_start;
	u_char* buffers_end;
	u_char* buffers_read;
	u_char* buffers_write;
	ngx_buffer_cache_stats_t stats;
} ngx_buffer_cache_t;

#endif // _NGX_BUFFER_CACHE_INTERNAL_H_INCLUDED_
