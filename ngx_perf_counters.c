#include "ngx_perf_counters.h"

#define LOG_CONTEXT_FORMAT " in perf counters \"%V\"%Z"

const ngx_str_t perf_counters_open_tags[] = {
#define PC(id, name) { sizeof(#name) - 1 + 4, (u_char*)("<" #name ">\r\n") },
#include "ngx_perf_counters_x.h"
#undef PC
};

const ngx_str_t perf_counters_close_tags[] = {
#define PC(id, name) { sizeof(#name) - 1 + 5, (u_char*)("</" #name ">\r\n") },
#include "ngx_perf_counters_x.h"
#undef PC
};

static ngx_int_t
ngx_perf_counters_init(ngx_shm_zone_t *shm_zone, void *data)
{
	ngx_perf_counters_t *state;
	ngx_slab_pool_t *shpool;
	u_char* p;

	if (data)
	{
		shm_zone->data = data;
		return NGX_OK;
	}

	shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

	if (shm_zone->shm.exists)
	{
		shm_zone->data = shpool->data;
		return NGX_OK;
	}

	// start following the ngx_slab_pool_t that was allocated at the beginning of the chunk
	p = shm_zone->shm.addr + sizeof(ngx_slab_pool_t);

	// initialize the log context
	shpool->log_ctx = p;
	p = ngx_sprintf(shpool->log_ctx, LOG_CONTEXT_FORMAT, &shm_zone->shm.name);

	// allocate the perf couonters state
	p = ngx_align_ptr(p, sizeof(ngx_atomic_t));
	state = (ngx_perf_counters_t*)p;

	ngx_memzero(state, sizeof(*state));

	shpool->data = state;

	return NGX_OK;
}

ngx_shm_zone_t*
ngx_perf_counters_create_zone(ngx_conf_t *cf, ngx_str_t *name, void *tag)
{
	ngx_shm_zone_t* result;
	size_t size;

	size = sizeof(ngx_slab_pool_t) + sizeof(LOG_CONTEXT_FORMAT) + name->len + sizeof(ngx_atomic_t) + sizeof(ngx_perf_counters_t);

	result = ngx_shared_memory_add(cf, name, size, tag);
	if (result == NULL)
	{
		return NULL;
	}

	result->init = ngx_perf_counters_init;
	return result;
}
