#include "buffer_pool.h"

// macros
#define next_buffer(buf) (*(void**)buf)

// typedefs
struct buffer_pool_s {
	size_t size;
	void* head;
};

typedef struct {
	buffer_pool_t* buffer_pool;
	void* buffer;
} buffer_pool_cleanup_t;

buffer_pool_t*
buffer_pool_create(vod_pool_t* pool, vod_log_t* log, size_t buffer_size, size_t count)
{
	buffer_pool_t* buffer_pool;
	u_char* cur_buffer;
	void* head;

	if ((buffer_size & 0x0F) != 0)
	{
		vod_log_error(VOD_LOG_ERR, log, 0,
			"buffer_pool_create: invalid size %uz must be a multiple of 16", buffer_size);
		return NULL;
	}

	buffer_pool = vod_alloc(pool, sizeof(*buffer_pool));
	if (buffer_pool == NULL) 
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, log, 0,
			"buffer_pool_create: vod_alloc failed (1)");
		return NULL;
	}

	cur_buffer = vod_alloc(pool, buffer_size * count);
	if (cur_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, log, 0,
			"buffer_pool_create: vod_alloc failed (2)");
		return NULL;
	}

	head = NULL;
	for (; count > 0; count--, cur_buffer += buffer_size)
	{
		next_buffer(cur_buffer) = head;
		head = cur_buffer;
	}

	buffer_pool->size = buffer_size;
	buffer_pool->head = head;

	return buffer_pool;
}

static void
buffer_pool_buffer_cleanup(void *data)
{
	buffer_pool_cleanup_t* c = data;
	buffer_pool_t* buffer_pool = c->buffer_pool;
	void* buffer = c->buffer;

	next_buffer(buffer) = buffer_pool->head;
	buffer_pool->head = buffer;
}

void*
buffer_pool_alloc(request_context_t* request_context, buffer_pool_t* buffer_pool, size_t* buffer_size)
{
	buffer_pool_cleanup_t* buf_cln;
	vod_pool_cleanup_t* cln;
	void* result;

	if (buffer_pool == NULL)
	{
		return vod_alloc(request_context->pool, *buffer_size);
	}

	if (buffer_pool->head == NULL)
	{
		*buffer_size = buffer_pool->size;
		return vod_alloc(request_context->pool, *buffer_size);
	}

	cln = vod_pool_cleanup_add(request_context->pool, sizeof(buffer_pool_cleanup_t));
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"buffer_pool_alloc: vod_pool_cleanup_add failed");
		return NULL;
	}

	result = buffer_pool->head;
	buffer_pool->head = next_buffer(result);

	cln->handler = buffer_pool_buffer_cleanup;

	buf_cln = cln->data;
	buf_cln->buffer = result;
	buf_cln->buffer_pool = buffer_pool;

	*buffer_size = buffer_pool->size;

	return result;
}
