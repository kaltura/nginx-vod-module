#include "ngx_file_reader.h"
#include <ngx_event.h>

ngx_int_t 
file_reader_init(
	file_reader_state_t* state, 
	async_read_callback_t callback,
	void* callback_context,
	ngx_http_request_t *r,
	ngx_http_core_loc_conf_t  *clcf, 
	ngx_str_t* path)
{
	ngx_open_file_info_t       of;
	ngx_uint_t                 level;
	ngx_int_t    rc;

	state->log = r->connection->log;

	state->r = r;
#if (NGX_HAVE_FILE_AIO)
	state->use_aio = clcf->aio;
	state->callback = callback;
	state->callback_context = callback_context;
#endif

	ngx_memzero(&of, sizeof(ngx_open_file_info_t));

	of.read_ahead = clcf->read_ahead;
	of.directio = NGX_MAX_OFF_T_VALUE;
	of.valid = clcf->open_file_cache_valid;
	of.min_uses = clcf->open_file_cache_min_uses;
	of.errors = clcf->open_file_cache_errors;
	of.events = clcf->open_file_cache_events;

	rc = ngx_http_set_disable_symlinks(r, clcf, path, &of);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"file_reader_init: ngx_http_set_disable_symlinks failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	if (ngx_open_cached_file(clcf->open_file_cache, path, &of, r->pool) != NGX_OK)
	{
		switch (of.err) 
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
#endif

			level = NGX_LOG_ERR;
			rc = NGX_HTTP_FORBIDDEN;
			break;

		default:

			level = NGX_LOG_CRIT;
			rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
			break;
		}

		if (rc != NGX_HTTP_NOT_FOUND || clcf->log_not_found) 
		{
			ngx_log_error(level, state->log, of.err, "file_reader_init: %s \"%s\" failed", of.failed, path->data);
		}

		return rc;
	}

	if (!of.is_file) 
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0, "file_reader_init: \"%s\" is not a file", path->data);
		if (ngx_close_file(of.fd) == NGX_FILE_ERROR)
		{
			ngx_log_error(NGX_LOG_ALERT, state->log, ngx_errno, "file_reader_init: " ngx_close_file_n " \"%s\" failed", path->data);
		}

		return NGX_HTTP_FORBIDDEN;
	}

	state->file.fd = of.fd;
	state->file.name = *path;
	state->file.log = state->log;
	state->file_size = of.size;
	state->use_directio = (clcf->directio <= of.size);

	return NGX_OK;
}

// Note: this function initializes r->exten in order to have nginx select the correct mime type for the request
//		the code was copied from nginx's ngx_http_set_exten
static void
file_reader_set_request_extension(ngx_http_request_t *r, ngx_str_t* path)
{
	ngx_int_t  i;

	ngx_str_null(&r->exten);

	for (i = path->len - 1; i > 1; i--) {
		if (path->data[i] == '.' && path->data[i - 1] != '/') {

			r->exten.len = path->len - i - 1;
			r->exten.data = &path->data[i + 1];

			return;

		}
		else if (path->data[i] == '/') {
			return;
		}
	}

	return;
}

ngx_int_t
file_reader_dump_file(file_reader_state_t* state)
{
	ngx_http_request_t* r = state->r;
	ngx_buf_t                 *b;
	ngx_int_t                  rc;
	ngx_chain_t                out;

	file_reader_set_request_extension(r, &state->file.name);

	file_reader_enable_directio(state);		// ignore errors

	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = state->file_size;

	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK) 
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"file_reader_dump_file: ngx_http_set_etag failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	rc = ngx_http_set_content_type(r);
	if (rc != NGX_OK) 
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"file_reader_dump_file: ngx_http_set_content_type failed %i", rc);
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
	if (b == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"file_reader_dump_file: ngx_pcalloc failed (1)");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
	if (b->file == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"file_reader_dump_file: ngx_pcalloc failed (2)");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) 
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"file_reader_dump_file: ngx_http_send_header failed %i", rc);
		return rc;
	}

	b->file_pos = 0;
	b->file_last = state->file_size;

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
			"file_reader_dump_file: ngx_http_output_filter failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t 
file_reader_enable_directio(file_reader_state_t* state)
{
	if (state->use_directio) 
	{
		if (ngx_directio_on(state->file.fd) == NGX_FILE_ERROR) 
		{
			ngx_log_error(NGX_LOG_ALERT, state->log, ngx_errno,
				"file_reader_enable_directio: " ngx_directio_on_n " \"%s\" failed", state->file.name.data);
			return NGX_FILE_ERROR;
		}

		state->file.directio = 1;
	}

	return NGX_OK;
}

#if (NGX_HAVE_FILE_AIO)

static void
async_read_completed_callback(ngx_event_t *ev)
{
	file_reader_state_t* state;
	ngx_event_aio_t     *aio;
	ngx_http_request_t  *r;
	ssize_t bytes_read;
	ssize_t rc;

	aio = ev->data;
	state = aio->data;
	r = state->r;

	r->main->blocked--;
	r->aio = 0;

	// get the number of bytes read (offset, size, buffer are ignored in this case)
	rc = ngx_file_aio_read(&state->file, NULL, 0, 0, r->pool);

	if (rc < 0)
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0,
			"async_read_completed_callback: ngx_file_aio_read failed rc=%z", rc);
		bytes_read = 0;
	}
	else
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0, "async_read_completed_callback: ngx_file_aio_read returned %z", rc);
		bytes_read = rc;
		rc = NGX_OK;
	}

	state->callback(state->callback_context, rc, bytes_read);
}

ssize_t 
async_file_read(file_reader_state_t* state, u_char *buf, size_t size, off_t offset)
{
	ssize_t rc;

	ngx_log_debug2(NGX_LOG_DEBUG_HTTP, state->log, 0, "async_file_read: reading offset %O size %uz", offset, size);

	if (state->use_aio)
	{
		rc = ngx_file_aio_read(&state->file, buf, size, offset, state->r->pool);
		if (rc == NGX_AGAIN)
		{
			// wait for completion
			state->file.aio->data = state;
			state->file.aio->handler = async_read_completed_callback;

			state->r->main->blocked++;
			state->r->aio = 1;
			return rc;
		}
	}
	else
	{
		rc = ngx_read_file(&state->file, buf, size, offset);
	}

	if (rc < 0)
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0, "async_file_read: ngx_file_aio_read failed rc=%z", rc);
	}
	else
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0, "async_file_read: ngx_file_aio_read returned %z", rc);
	}

	return rc;
}

#else

ssize_t 
async_file_read(file_reader_state_t* state, u_char *buf, size_t size, off_t offset)
{
	ssize_t rc;

	ngx_log_debug2(NGX_LOG_DEBUG_HTTP, state->log, 0, "async_file_read: reading offset %O size %uz", offset, size);

	rc = ngx_read_file(&state->file, buf, size, offset);
	if (rc < 0)
	{
		ngx_log_error(NGX_LOG_ERR, state->log, 0, "async_file_read: ngx_read_file failed rc=%z", rc);
	}
	else
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0, "async_file_read: ngx_read_file returned %z", rc);
	}

	return rc;
}

#endif
