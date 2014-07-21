#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include <ngx_event.h>

#include "ngx_child_http_request.h"
#include "ngx_http_vod_module.h"

static ngx_str_t  child_http_hide_headers[] = {
	ngx_string("Date"),
	ngx_string("Server"),
	ngx_null_string
};

static ngx_str_t empty_str = ngx_null_string;

// constants
static const char content_length_header[] = "content-length";

static ngx_int_t
ngx_http_vod_create_request(ngx_http_request_t *r)
{
	ngx_chain_t                    *cl;
	child_request_context_t* ctx;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_create_request started");
	
	// allocate a chain and associate the previously created request buffer
	cl = ngx_alloc_chain_link(r->pool);
	if (cl == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_create_request: ngx_alloc_chain_link failed");
		return NGX_ERROR;
	}

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	cl->buf = ctx->request_buffer;
	cl->next = NULL;

	r->upstream->request_bufs = cl;

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_create_request done %s", ctx->request_buffer->pos);

	return NGX_OK;
}

static void
ngx_http_vod_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
	child_request_context_t* ctx;

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_finalize_request: started rc=%i", rc);

	// Note: not doing anything in case of error since nginx terminates the parent request automatically with error 502

	// notify the caller
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx->callback != NULL && rc == NGX_OK)
	{
		// call the callback in a deferred fashion since the callback may finalize the request, 
		// and the upstream module uses the request object after this function returns
		ngx_add_timer(ctx->complete_event, 0);
	}
	else
	{
		// reset the request state
		r->subrequest_in_memory = 0;
		r->headers_out.status_line.len = 0;
		r->main->blocked--;
	}

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_finalize_request done");
}

static ngx_int_t
ngx_http_vod_reinit_request(ngx_http_request_t *r)
{
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_reinit_request started");

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_process_header(ngx_http_request_t *r)
{
	child_request_context_t* ctx;
	ngx_http_upstream_t *u;
	ngx_table_elt_t *h;
	ngx_int_t rc;
	off_t read_body_size;

	u = r->upstream;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	for (;;) 
	{
		rc = ngx_http_parse_header_line(r, &u->buffer, 1);
		if (rc == NGX_OK)	// a header line has been parsed successfully
		{
			if (ctx->save_response_headers)
			{
				h = ngx_list_push(&r->upstream->headers_in.headers);
				if (h == NULL) 
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_process_header: ngx_list_push failed");
					return NGX_ERROR;
				}

				h->hash = r->header_hash;

				h->key.len = r->header_name_end - r->header_name_start;
				h->value.len = r->header_end - r->header_start;

				h->key.data = ngx_pnalloc(r->pool, h->key.len + 1 + h->value.len + 1 + h->key.len);
				if (h->key.data == NULL) 
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_process_header: ngx_pnalloc failed (1)");
					return NGX_ERROR;
				}

				h->value.data = h->key.data + h->key.len + 1;
				h->lowcase_key = h->key.data + h->key.len + 1 + h->value.len + 1;

				ngx_memcpy(h->key.data, r->header_name_start, h->key.len);
				h->key.data[h->key.len] = '\0';
				ngx_memcpy(h->value.data, r->header_start, h->value.len);
				h->value.data[h->value.len] = '\0';

				if (h->key.len == r->lowcase_index) 
				{
					ngx_memcpy(h->lowcase_key, r->lowcase_header, h->key.len);
				}
				else 
				{
					ngx_strlow(h->lowcase_key, h->key.data, h->key.len);
				}
			}

			// parse the content length
			if (r->header_name_end - r->header_name_start == sizeof(content_length_header) - 1 &&
				ngx_memcmp(r->lowcase_header, content_length_header, sizeof(content_length_header) - 1) == 0)
			{
				u->headers_in.content_length_n = ngx_atoof(r->header_start, r->header_end - r->header_start);
				if (u->headers_in.content_length_n < 0) 
				{
					ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
						"ngx_http_vod_process_header: failed to parse content length");
					return NGX_HTTP_UPSTREAM_INVALID_HEADER;
				}

				if (u->headers_in.content_length_n > ctx->response_buffer_size)
				{
					ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
						"ngx_http_vod_process_header: content length %O exceeds the limit %O", u->headers_in.content_length_n, ctx->response_buffer_size);
					return NGX_HTTP_UPSTREAM_INVALID_HEADER;
				}

				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
					"ngx_http_vod_process_header: parsed content length %O", u->headers_in.content_length_n);
			}

			continue;
		}

		if (rc == NGX_HTTP_PARSE_HEADER_DONE)	// finished reading all headers
		{
			// make sure we got some content length
			if (u->headers_in.content_length_n < 0)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_process_header: got no content-length header");
				return NGX_HTTP_UPSTREAM_INVALID_HEADER;
			}

			// allocate a response buffer if needed
			if (ctx->response_buffer == NULL)
			{
				ctx->response_buffer = ngx_palloc(r->pool, u->headers_in.content_length_n + 1);
				if (ctx->response_buffer == NULL)
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_process_header: ngx_pnalloc failed (2)");
					return NGX_ERROR;
				}

				ctx->response_buffer[u->headers_in.content_length_n] = '\0';
			}

			// in case we already got some of the response body, copy it to the response buffer
			read_body_size = u->buffer.last - u->buffer.pos;
			if (read_body_size > ctx->response_buffer_size)
			{
				read_body_size = ctx->response_buffer_size;
			}
			ngx_memcpy(ctx->response_buffer, u->buffer.pos, read_body_size);

			// set the upstream buffer to our response buffer
			u->buffer.start = ctx->response_buffer;
			u->buffer.pos = u->buffer.start;
			u->buffer.last = u->buffer.start + read_body_size;
			u->buffer.end = u->buffer.start + ctx->response_buffer_size;
			u->buffer.temporary = 1;

			return NGX_OK;
		}

		if (rc == NGX_AGAIN)		// need more data
		{
			return NGX_AGAIN;
		}

		// there was error while a header line parsing
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_process_header: failed to parse header line rc=%i", rc);
		return NGX_HTTP_UPSTREAM_INVALID_HEADER;
	}
}

static ngx_int_t
ngx_http_vod_process_status_line(ngx_http_request_t *r)
{
	ngx_http_upstream_t *u;
	ngx_http_status_t status;
	size_t len;
	ngx_int_t rc;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_process_status_line started");

	u = r->upstream;

	ngx_memzero(&status, sizeof(status));
	rc = ngx_http_parse_status_line(r, &u->buffer, &status);
	if (rc == NGX_AGAIN) 
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_process_status_line: ngx_http_parse_status_line failed %i", rc);
		return rc;
	}

	if (rc == NGX_ERROR) 
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_process_status_line: failed to parse status line");
		return NGX_ERROR;
	}

	if (u->state && u->state->status == 0) 
	{
		u->state->status = status.code;
	}

	u->headers_in.status_n = status.code;

	len = status.end - status.start;
	u->headers_in.status_line.len = len;

	u->headers_in.status_line.data = ngx_pnalloc(r->pool, len);
	if (u->headers_in.status_line.data == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_process_status_line: ngx_pnalloc failed");
		return NGX_ERROR;
	}

	ngx_memcpy(u->headers_in.status_line.data, status.start, len);

	if (status.http_version < NGX_HTTP_VERSION_11) 
	{
		u->headers_in.connection_close = 1;
	}

	// change state to processing the rest of the headers
	u->process_header = ngx_http_vod_process_header;

	return ngx_http_vod_process_header(r);
}

static void
ngx_http_vod_abort_request(ngx_http_request_t *r)
{
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_abort_request started");
	return;
}

static ngx_int_t
ngx_http_vod_filter_init(void *data)
{
	child_request_context_t* ctx;
	ngx_http_request_t   *r = data;
	ngx_http_upstream_t  *u;

	u = r->upstream;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	if (u->headers_in.content_length_n > u->buffer.end - u->buffer.start) 
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_filter_init: content length %O exceeds buffer size %O", u->headers_in.content_length_n, (off_t)(u->buffer.end - u->buffer.start));
		return NGX_ERROR;
	}

	u->length = u->headers_in.content_length_n;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_filter(void *data, ssize_t bytes)
{
	ngx_http_request_t   *r = data;
	ngx_http_upstream_t  *u;
	ngx_buf_t            *b;

	u = r->upstream;
	b = &u->buffer;

	// only need to update the length and buffer position
	b->last += bytes;
	u->length -= bytes;

	return NGX_OK;
}

static ngx_int_t
create_upstream(
	ngx_http_request_t *r, 
	ngx_http_upstream_conf_t* conf)
{
	ngx_http_upstream_t *u;
	ngx_int_t rc;

	// create the upstream
	rc = ngx_http_upstream_create(r);
	if (rc != NGX_OK)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"create_upstream: failed to create upstream rc=%i", rc);
		return NGX_ERROR;
	}

	// initialize the upstream
	u = r->upstream;

	ngx_str_set(&u->schema, "kalapi://");
	u->output.tag = (ngx_buf_tag_t)&ngx_http_vod_module;
	u->buffer.tag = u->output.tag;

	u->conf = conf;

	u->create_request = ngx_http_vod_create_request;
	u->reinit_request = ngx_http_vod_reinit_request;
	u->process_header = ngx_http_vod_process_status_line;
	u->abort_request = ngx_http_vod_abort_request;
	u->finalize_request = ngx_http_vod_finalize_request;

	return NGX_OK;
}

static ngx_int_t
init_request_buffer(
	ngx_http_request_t *r,
	child_request_context_t* ctx,
	ngx_str_t* base_uri,
	ngx_str_t* extra_args,
	ngx_str_t* host_name,
	off_t range_start,
	off_t range_end,
	ngx_str_t* extra_headers)
{
	ngx_flag_t range_request = (range_start >= 0) && (range_end >= 0);
	ngx_buf_t *b;
	size_t len;
	u_char* p;

	// calculate the request size
	len =
		sizeof("GET ") - 1 + base_uri->len + sizeof("?") - 1 + extra_args->len + sizeof(" HTTP/1.1" CRLF) - 1 +
		sizeof("Host: ") - 1 + host_name->len + sizeof(CRLF) - 1 +
		extra_headers->len +
		sizeof(CRLF);
	if (range_request)
	{
		len += sizeof("Range: bytes=") - 1 + NGX_INT64_LEN + sizeof("-") - 1 + NGX_INT64_LEN + sizeof(CRLF) - 1;
	}

	// get/allocate the request buffer
	if (ctx->request_buffer != NULL && len <= (size_t)(ctx->request_buffer->end - ctx->request_buffer->start))
	{
		b = ctx->request_buffer;
		b->pos = b->start;
		p = b->start;
	}
	else
	{
		b = ngx_create_temp_buf(r->pool, len);
		if (b == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"init_request_buffer: ngx_create_temp_buf failed");
			return NGX_ERROR;
		}
		p = b->last;
	}

	// first header line
	*p++ = 'G';		*p++ = 'E';		*p++ = 'T';		*p++ = ' ';
	p = ngx_copy(p, base_uri->data, base_uri->len);
	if (extra_args->len > 0)
	{
		if (ngx_strchr(base_uri->data, '?'))
		{
			*p++ = '&';
		}
		else
		{
			*p++ = '?';
		}

		p = ngx_copy(p, extra_args->data, extra_args->len);
	}
	p = ngx_copy(p, " HTTP/1.1" CRLF, sizeof(" HTTP/1.1" CRLF) - 1);

	// host line
	p = ngx_copy(p, "Host: ", sizeof("Host: ") - 1);
	p = ngx_copy(p, host_name->data, host_name->len);
	*p++ = '\r';	*p++ = '\n';

	// range request
	if (range_request)
	{
		p = ngx_sprintf(p, "Range: bytes=%O-%O" CRLF, range_start, range_end);
	}

	// additional headers
	p = ngx_copy(p, extra_headers->data, extra_headers->len);

	// headers end
	*p++ = '\r';	*p++ = '\n';
	*p = '\0';
	b->last = p;

	ctx->request_buffer = b;

	return NGX_OK;
}

ngx_int_t
dump_request(
	ngx_http_request_t *r,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* uri,
	ngx_str_t* host_name,
	ngx_str_t* extra_headers)
{
	child_request_context_t* ctx;
	ngx_http_upstream_t *u;
	ngx_int_t rc;

	// save input params
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	ctx->callback = NULL;
	ctx->save_response_headers = 1;

	rc = create_upstream(r, upstream_conf);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"dump_request: create_upstream failed %i", rc);
		return rc;
	}

	u = r->upstream;

	rc = init_request_buffer(r, ctx, uri, &empty_str, host_name, -1, -1, extra_headers);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"dump_request: init_request_buffer failed %i", rc);
		return rc;
	}

	r->main->blocked++;

	// start the request
	ngx_http_upstream_init(r);

	return NGX_AGAIN;
}

static void
event_callback(ngx_event_t *ev)
{
	ngx_http_request_t *r = ev->data;
	child_request_context_t* ctx;

	// reset the request state
	r->subrequest_in_memory = 0;
	r->headers_out.status_line.len = 0;
	r->main->blocked--;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	ctx->callback(ctx->callback_context, &r->upstream->buffer);
}

static void
child_request_del_timer(child_request_context_t* ctx)
{
	if (ctx->complete_event->timer_set)
	{
		ngx_del_timer(ctx->complete_event);
	}
}

// Note: must not return positive error codes (like NGX_HTTP_INTERNAL_SERVER_ERROR)
ngx_int_t
child_request_start(
	ngx_http_request_t *r,
	child_request_callback_t callback,
	void* callback_context,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* base_uri,
	ngx_str_t* extra_args,
	ngx_str_t* host_name,
	off_t range_start,
	off_t range_end,
	off_t max_response_length,
	u_char* response_buffer)
{
	child_request_context_t* ctx;
	ngx_http_upstream_t *u;
	ngx_int_t rc;

	// save input params
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	ctx->callback = callback;
	ctx->callback_context = callback_context;
	ctx->response_buffer_size = max_response_length;
	ctx->response_buffer = response_buffer;
	ctx->save_response_headers = 0;

	rc = create_upstream(r, upstream_conf);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"child_request_start: create_upstream failed %i", rc);
		return rc;
	}

	if (ctx->complete_event == NULL)
	{
		ctx->complete_event = ngx_pcalloc(r->pool, sizeof(*ctx->complete_event));
		if (ctx->complete_event == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"child_request_start: ngx_pcalloc failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		ctx->complete_event->handler = event_callback;
		ctx->complete_event->data = r;
		ctx->complete_event->log = r->connection->log;
	}

	if (ctx->cleanup == NULL)
	{
		ctx->cleanup = ngx_pool_cleanup_add(r->pool, 0);
		if (ctx->cleanup == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"child_request_start: ngx_pool_cleanup_add failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		ctx->cleanup->handler = (ngx_pool_cleanup_pt)child_request_del_timer;
		ctx->cleanup->data = ctx;
	}

	if (ctx->headers_buffer == NULL || ctx->headers_buffer_size < upstream_conf->buffer_size)
	{
		if (ctx->headers_buffer != NULL)
		{
			ngx_pfree(r->pool, ctx->headers_buffer);
		}

		ctx->headers_buffer_size = upstream_conf->buffer_size;
		ctx->headers_buffer = ngx_palloc(r->pool, ctx->headers_buffer_size);
		if (ctx->headers_buffer == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"child_request_start: ngx_palloc failed");
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
	}

	u = r->upstream;

	u->buffer.start = ctx->headers_buffer;
	u->buffer.pos = u->buffer.start;
	u->buffer.last = u->buffer.start;
	u->buffer.end = u->buffer.start + ctx->headers_buffer_size;
	u->buffer.temporary = 1;

	if (ngx_list_init(&u->headers_in.headers, r->pool, 8, sizeof(ngx_table_elt_t)) != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"child_request_start: ngx_list_init failed");
		return NGX_ERROR;
	}

	u->input_filter_init = ngx_http_vod_filter_init;
	u->input_filter = ngx_http_vod_filter;
	u->input_filter_ctx = r;

	rc = init_request_buffer(r, ctx, base_uri, extra_args, host_name, range_start, range_end, &empty_str);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"child_request_start: init_request_buffer failed %i", rc);
		return rc;
	}

	// make the upstream save the contents in memory instead of passing to the client
	r->subrequest_in_memory = 1;

	r->main->blocked++;

	// start the request
	ngx_http_upstream_init(r);

	return NGX_AGAIN;
}

void
child_request_free(ngx_http_request_t *r)
{
	child_request_context_t* ctx;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	ngx_pfree(r->pool, ctx->headers_buffer);
	ctx->headers_buffer = NULL;
}

void 
init_upstream_conf(ngx_http_upstream_conf_t* upstream)
{
	upstream->connect_timeout = NGX_CONF_UNSET_MSEC;
	upstream->send_timeout = NGX_CONF_UNSET_MSEC;
	upstream->read_timeout = NGX_CONF_UNSET_MSEC;

	upstream->buffer_size = NGX_CONF_UNSET_SIZE;

	upstream->hide_headers = NGX_CONF_UNSET_PTR;
	upstream->pass_headers = NGX_CONF_UNSET_PTR;

	// hardcoded values
	upstream->cyclic_temp_file = 0;
	upstream->buffering = 0;
	upstream->ignore_client_abort = 0;
	upstream->send_lowat = 0;
	upstream->bufs.num = 0;
	upstream->busy_buffers_size = 0;
	upstream->max_temp_file_size = 0;
	upstream->temp_file_write_size = 0;
	upstream->intercept_errors = 1;
	upstream->intercept_404 = 1;
	upstream->pass_request_headers = 0;
	upstream->pass_request_body = 0;
}

char *
merge_upstream_conf(
	ngx_conf_t *cf,
	ngx_http_upstream_conf_t* conf_upstream, 
	ngx_http_upstream_conf_t* prev_upstream)
{
	ngx_hash_init_t             hash;

	ngx_conf_merge_msec_value(conf_upstream->connect_timeout,
		prev_upstream->connect_timeout, 60000);

	ngx_conf_merge_msec_value(conf_upstream->send_timeout,
		prev_upstream->send_timeout, 60000);

	ngx_conf_merge_msec_value(conf_upstream->read_timeout,
		prev_upstream->read_timeout, 60000);

	ngx_conf_merge_size_value(conf_upstream->buffer_size,
		prev_upstream->buffer_size,
		(size_t)ngx_pagesize);

	ngx_conf_merge_bitmask_value(conf_upstream->next_upstream,
		prev_upstream->next_upstream,
		(NGX_CONF_BITMASK_SET
		| NGX_HTTP_UPSTREAM_FT_ERROR
		| NGX_HTTP_UPSTREAM_FT_TIMEOUT));

	if (conf_upstream->next_upstream & NGX_HTTP_UPSTREAM_FT_OFF) 
	{
		conf_upstream->next_upstream = NGX_CONF_BITMASK_SET | NGX_HTTP_UPSTREAM_FT_OFF;
	}

	if (conf_upstream->upstream == NULL) 
	{
		conf_upstream->upstream = prev_upstream->upstream;
	}

	hash.max_size = 512;
	hash.bucket_size = 64;
	hash.name = "child_request_headers_hash";

	if (ngx_http_upstream_hide_headers_hash(cf, conf_upstream,
		prev_upstream, child_http_hide_headers, &hash)
		!= NGX_OK)
	{
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}
