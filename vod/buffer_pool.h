#ifndef __BUFFER_POOL_H__
#define __BUFFER_POOL_H__

// includes
#include "common.h"

// functions
buffer_pool_t* buffer_pool_create(vod_pool_t* pool, vod_log_t* log, size_t buffer_size, size_t count);
void* buffer_pool_alloc(request_context_t* reqeust_context, buffer_pool_t* buffer_pool, size_t* buffer_size);

#endif // __BUFFER_POOL_H__
