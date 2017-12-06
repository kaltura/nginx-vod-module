#include "ngx_file_reader.h"
#include <ngx_event.h>

static ngx_int_t
ngx_file_reader_init_open_file_info(
	ngx_open_file_info_t* of, 
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t *clcf, 
	ngx_str_t* path)
{
	ngx_int_t rc;

	ngx_memzero(of, sizeof(ngx_open_file_info_t));

	of->read_ahead = clcf->read_ahead;
	of->directio = NGX_MAX_OFF_T_VALUE;
	of->valid = clcf->open_file_cache_valid;
	of->min_uses = clcf->open_file_cache_min_uses;
	of->errors = clcf->open_file_cache_errors;
	of->events = clcf->open_file_cache_events;

	rc = ngx_http_set_disable_symlinks(r, clcf, path, of);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_file_reader_init_open_file_info: ngx_http_set_disable_symlinks failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	return rc;
}

static ngx_int_t
ngx_file_reader_update_state_file_info(ngx_file_reader_state_t* state, ngx_open_file_info_t* of, ngx_int_t rc)
{
	ngx_uint_t level;

	if (rc != NGX_OK)
	{
		switch (of->err)
		{
		case 0:
			return NGX_HTTP_INTERNAL_SERVER_ERROR;

		case NGX_ENOENT:
		case NGX_ENOTDIR:
		case NGX_ENAMETOOLONG:

			level = NGX_LOG_ERR;
			rc = NGX_HTTP_NOT_FOUND;
			break;

		case NGX_EACCES:
#if (NGX_HAVE_OPENAT)
		case NGX_EMLINK:
		case NGX_ELOOP:
#endif // NGX_HAVE_OPENAT

			level = NGX_LOG_ERR;
			rc = NGX_HTTP_FORBIDDEN;
			break;

		default:

			level = NGX_LOG_CRIT;
			rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
			break;
		}

		if (rc != NGX_HTTP_NOT_FOUND || state->log_not_found)
		{
			ngx_log_error(level, state->log, of->err, "ngx_file_reader_update_state_file_info: %s \"%s\" failed", of->failed, state->file.name.data);
		}

		return rc;
	}

	if (!of->is_file)
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0, "ngx_file_reader_update_state_file_info: \"%s\" is not a file", state->file.name.data);
		if (of->fd != NGX_INVALID_FILE)
		{
			if (ngx_close_file(of->fd) == NGX_FILE_ERROR)
			{
				ngx_log_error(NGX_LOG_ALERT, state->log, ngx_errno, "ngx_file_reader_update_state_file_info: " ngx_close_file_n " \"%s\" failed", state->file.name.data);
			}
		}

		return NGX_HTTP_FORBIDDEN;
	}

	state->file.fd = of->fd;
	state->file_size = of->size;

	return NGX_OK;
}

ngx_int_t
ngx_file_reader_init(
	ngx_file_reader_state_t* state,
	ngx_async_read_callback_t read_callback,
	void* callback_context,
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t *clcf,
	ngx_str_t* path,
	uint32_t flags)
{
	ngx_open_file_info_t of;
	ngx_int_t rc;

	state->r = r;
	state->file.name = *path;
	state->file.log = r->connection->log;
	state->directio = clcf->directio;
	state->log_not_found = clcf->log_not_found;
	state->log = r->connection->log;
#if (NGX_HAVE_FILE_AIO)
	state->use_aio = clcf->aio;
	state->read_callback = read_callback;
	state->callback_context = callback_context;
#endif // NGX_HAVE_FILE_AIO

	rc = ngx_file_reader_init_open_file_info(&of, r, clcf, path);
	if (rc != NGX_OK)
	{
		return rc;
	}

	rc = ngx_open_cached_file(
		(flags & OPEN_FILE_NO_CACHE) != 0 ? NULL : clcf->open_file_cache, 
		path, 
		&of, 
		r->pool);

	return ngx_file_reader_update_state_file_info(state, &of, rc);
}

#if (NGX_THREADS)

typedef struct {
	ngx_file_reader_state_t* state;
	ngx_open_file_info_t of;
	ngx_async_open_file_callback_t open_callback;
	void* callback_context;
	ngx_thread_task_t *task;
} ngx_file_reader_async_open_context_t;

static void
ngx_file_reader_async_open_callback(void* ctx, ngx_int_t rc)
{
	ngx_file_reader_async_open_context_t* context = ctx;
	ngx_file_reader_state_t* state = context->state;
	ngx_http_request_t *r = state->r;
	ngx_connection_t *c = r->connection;

	r->main->blocked--;
	r->aio = 0;

	rc = ngx_file_reader_update_state_file_info(state, &context->of, rc);

	context->open_callback(context->callback_context, rc);

	ngx_http_run_posted_requests(c);
}

ngx_int_t 
ngx_file_reader_init_async(
	ngx_file_reader_state_t* state,
	void** context,
	ngx_thread_pool_t *thread_pool,
	ngx_async_open_file_callback_t open_callback,
	ngx_async_read_callback_t read_callback,
	void* callback_context,
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t *clcf,
	ngx_str_t* path,
	uint32_t flags)
{
	ngx_file_reader_async_open_context_t* open_context;
	ngx_int_t rc;

	state->r = r;
	state->file.name = *path;
	state->file.log = r->connection->log;
	state->directio = clcf->directio;
	state->log_not_found = clcf->log_not_found;
	state->log = r->connection->log;
#if (NGX_HAVE_FILE_AIO)
	state->use_aio = clcf->aio;
	state->read_callback = read_callback;
	state->callback_context = callback_context;
#endif // NGX_HAVE_FILE_AIO

	open_context = *context;

	if (open_context == NULL)
	{
		open_context = ngx_palloc(r->pool, sizeof(ngx_file_reader_async_open_context_t));
		if (open_context == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, state->log, 0,
				"ngx_file_reader_init_async: ngx_palloc failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		open_context->task = NULL;		// all other fields explicitly set below

		*context = open_context;
	}

	open_context->state = state;
	open_context->open_callback = open_callback;
	open_context->callback_context = callback_context;

	rc = ngx_file_reader_init_open_file_info(&open_context->of, r, clcf, path);
	if (rc != NGX_OK)
	{
		return rc;
	}

	rc = ngx_async_open_cached_file(
		(flags & OPEN_FILE_NO_CACHE) != 0 ? NULL : clcf->open_file_cache, 
		path,
		&open_context->of,
		r->pool,
		thread_pool,
		&open_context->task,
		ngx_file_reader_async_open_callback,
		open_context);
	if (rc == NGX_AGAIN)
	{
		r->main->blocked++;
		r->aio = 1;

		return NGX_AGAIN;
	}

	return ngx_file_reader_update_state_file_info(state, &open_context->of, rc);
}

#endif // NGX_THREADS

ngx_int_t
ngx_file_reader_dump_file_part(void* context, off_t start, off_t end)
{
	ngx_file_reader_state_t* state = context;
	ngx_http_request_t* r = state->r;
	ngx_buf_t                 *b;
	ngx_int_t                  rc;
	ngx_chain_t                out;

	b = ngx_calloc_buf(r->pool);
	if (b == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"ngx_file_reader_dump_file_part: ngx_pcalloc failed (1)");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
	if (b->file == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"ngx_file_reader_dump_file_part: ngx_pcalloc failed (2)");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b->file_pos = start;
	if (end != 0)
	{
		if (end > state->file_size)
		{
			ngx_log_error(NGX_LOG_ERR, state->log, ngx_errno,
				"ngx_file_reader_dump_file_part: end offset %O exceeds file size %O, probably a truncated file", end, state->file_size);
			return NGX_HTTP_NOT_FOUND;
		}
		b->file_last = end;
	}
	else
	{
		b->file_last = state->file_size;
	}

	b->in_file = b->file_last ? 1 : 0;
	b->last_buf = (r == r->main) ? 1 : 0;
	b->last_in_chain = 1;

	b->file->fd = state->file.fd;
	b->file->name = state->file.name;
	b->file->log = state->log;
	b->file->directio = state->file.directio;

	out.buf = b;
	out.next = NULL;

	rc = ngx_http_output_filter(r, &out);
	if (rc != NGX_OK && rc != NGX_AGAIN)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"ngx_file_reader_dump_file_part: ngx_http_output_filter failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t
ngx_file_reader_enable_directio(ngx_file_reader_state_t* state)
{
	if (state->directio <= state->file_size)
	{
		if (ngx_directio_on(state->file.fd) == NGX_FILE_ERROR) 
		{
			ngx_log_error(NGX_LOG_ALERT, state->log, ngx_errno,
				"ngx_file_reader_enable_directio: " ngx_directio_on_n " \"%s\" failed", state->file.name.data);
			return NGX_FILE_ERROR;
		}

		state->file.directio = 1;
	}

	return NGX_OK;
}

size_t 
ngx_file_reader_get_size(void* context)
{
	ngx_file_reader_state_t* state = context;

	return state->file_size;
}

void
ngx_file_reader_get_path(void* context, ngx_str_t* path)
{
	ngx_file_reader_state_t* ctx = context;

	*path = ctx->file.name;
}

#if (NGX_HAVE_FILE_AIO)

static void
ngx_async_read_completed_callback(ngx_event_t *ev)
{
	ngx_file_reader_state_t* state;
	ngx_http_request_t *r;
	ngx_connection_t *c;
	ngx_event_aio_t *aio;
	ssize_t bytes_read;
	ssize_t rc;

	aio = ev->data;
	state = aio->data;
	r = state->r;
	c = r->connection;

	r->main->blocked--;
	r->aio = 0;

	// get the number of bytes read (offset, size, buffer are ignored in this case)
	rc = ngx_file_aio_read(&state->file, NULL, 0, 0, r->pool);

	if (rc < 0)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"ngx_async_read_completed_callback: ngx_file_aio_read failed rc=%z", rc);
		bytes_read = 0;
	}
	else
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0, "ngx_async_read_completed_callback: ngx_file_aio_read returned %z", rc);
		state->buf->last += rc;
		bytes_read = rc;
		rc = NGX_OK;
	}

	state->read_callback(state->callback_context, rc, NULL, bytes_read);

	ngx_http_run_posted_requests(c);
}

ngx_int_t 
ngx_async_file_read(ngx_file_reader_state_t* state, ngx_buf_t *buf, size_t size, off_t offset)
{
	ssize_t rc;

	ngx_log_debug2(NGX_LOG_DEBUG_HTTP, state->log, 0, "ngx_async_file_read: reading offset %O size %uz", offset, size);

	if (state->use_aio)
	{
		rc = ngx_file_aio_read(&state->file, buf->last, size, offset, state->r->pool);
		if (rc == NGX_AGAIN)
		{
			// wait for completion
			state->file.aio->data = state;
			state->file.aio->handler = ngx_async_read_completed_callback;

			state->r->main->blocked++;
			state->r->aio = 1;

			state->buf = buf;
			return rc;
		}
	}
	else
	{
		rc = ngx_read_file(&state->file, buf->last, size, offset);
	}

	if (rc < 0)
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0, "ngx_async_file_read: ngx_file_aio_read failed rc=%z", rc);
		return rc;
	}

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0, "ngx_async_file_read: ngx_file_aio_read returned %z", rc);
	buf->last += rc;
	
	return NGX_OK;
}

#else

ngx_int_t 
ngx_async_file_read(ngx_file_reader_state_t* state, ngx_buf_t *buf, size_t size, off_t offset)
{
	ssize_t rc;

	ngx_log_debug2(NGX_LOG_DEBUG_HTTP, state->log, 0, "ngx_async_file_read: reading offset %O size %uz", offset, size);

	rc = ngx_read_file(&state->file, buf->last, size, offset);
	if (rc < 0)
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0, "ngx_async_file_read: ngx_read_file failed rc=%z", rc);
		return rc;
	}

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0, "ngx_async_file_read: ngx_read_file returned %z", rc);
	buf->last += rc;

	return NGX_OK;
}

#endif // NGX_HAVE_FILE_AIO
