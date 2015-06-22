#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include <ngx_event.h>

#include "ngx_child_http_request.h"
#include "ngx_http_vod_module.h"

// macros
#define is_dump_request(r) (r == r->main)		// regular requests use subrequests

// typedefs
typedef struct {
	ngx_buf_t* request_buffer;
	ngx_http_status_t status;
} ngx_child_request_base_context_t;		// fields common to dump requests and child requests

typedef struct {
	ngx_child_request_base_context_t base;		// must be first

	// fixed
	ngx_http_upstream_conf_t* upstream_conf;
	ngx_child_request_callback_t callback;
	void* callback_context;
	ngx_flag_t in_memory;

	// in memory only
	u_char* headers_buffer;
	size_t headers_buffer_size;
	off_t read_body_size;
	u_char* response_buffer;
	off_t response_buffer_size;

	// temporary completion state
	ngx_http_upstream_t *upstream;
	void *original_context;
	ngx_int_t error_code;
	ngx_http_event_handler_pt original_write_event_handler;

} ngx_child_request_context_t;

// globals
static ngx_str_t child_http_hide_headers[] = {
	ngx_string("Date"),
	ngx_string("Server"),
	ngx_null_string
};

// constants
static const char content_length_header[] = "content-length";

static ngx_int_t
ngx_http_vod_create_request(ngx_http_request_t *r)
{
	ngx_chain_t *cl;
	ngx_child_request_base_context_t* ctx;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_create_request: started");
	
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

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_create_request: done %s", ctx->request_buffer->pos);

	return NGX_OK;
}

static void
ngx_http_vod_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_finalize_request: called rc=%i", rc);
}

static ngx_int_t
ngx_http_vod_reinit_request(ngx_http_request_t *r)
{
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_reinit_request: called");

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_process_header(ngx_http_request_t *r)
{
	ngx_child_request_context_t* ctx;
	ngx_http_upstream_t *u;
	ngx_table_elt_t *h;
	ngx_int_t rc;

	u = r->upstream;

	// Note: ctx must not be used in case of a dump request
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	for (;;) 
	{
		rc = ngx_http_parse_header_line(r, &u->buffer, 1);
		if (rc == NGX_OK)	// a header line has been parsed successfully
		{
			if (is_dump_request(r))
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

				// Note: not validating the content length in case of dump, since we don't load the whole response into memory
				if (!is_dump_request(r) && ctx->in_memory && u->headers_in.content_length_n > ctx->response_buffer_size)
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
			// in case of ngx_dump_request, no need to do anything
			if (is_dump_request(r))
			{
				return NGX_OK;
			}

			if (u->state && u->state->status != NGX_HTTP_OK && u->state->status != NGX_HTTP_PARTIAL_CONTENT)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_process_header: upstream returned a bad status %ui", u->state->status);
				return NGX_HTTP_UPSTREAM_INVALID_HEADER;
			}

			if (!ctx->in_memory)
			{
				return NGX_OK;
			}

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
				ctx->response_buffer_size = u->headers_in.content_length_n;
				ctx->response_buffer = ngx_palloc(r->pool, ctx->response_buffer_size + 1);
				if (ctx->response_buffer == NULL)
				{
					ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"ngx_http_vod_process_header: ngx_pnalloc failed (2)");
					return NGX_ERROR;
				}
			}

			// in case we already got some of the response body, copy it to the response buffer
			ctx->read_body_size = u->buffer.last - u->buffer.pos;
			if (ctx->read_body_size > ctx->response_buffer_size)
			{
				ctx->read_body_size = ctx->response_buffer_size;
			}
			ngx_memcpy(ctx->response_buffer, u->buffer.pos, ctx->read_body_size);

			// set the upstream buffer to our response buffer
			u->buffer.start = ctx->response_buffer;
			u->buffer.pos = u->buffer.start;
			u->buffer.last = u->buffer.start + ctx->read_body_size;
			u->buffer.end = u->buffer.start + ctx->response_buffer_size + 1;
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
	ngx_child_request_base_context_t* ctx;
	ngx_http_upstream_t *u;
	size_t len;
	ngx_int_t rc;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_process_status_line: started");

	u = r->upstream;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	rc = ngx_http_parse_status_line(r, &u->buffer, &ctx->status);
	if (rc == NGX_AGAIN) 
	{
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
		u->state->status = ctx->status.code;
	}

	u->headers_in.status_n = ctx->status.code;

	len = ctx->status.end - ctx->status.start;
	u->headers_in.status_line.len = len;

	u->headers_in.status_line.data = ngx_pnalloc(r->pool, len);
	if (u->headers_in.status_line.data == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_process_status_line: ngx_pnalloc failed");
		return NGX_ERROR;
	}

	ngx_memcpy(u->headers_in.status_line.data, ctx->status.start, len);

	if (ctx->status.http_version < NGX_HTTP_VERSION_11)
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
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ngx_http_vod_abort_request: called");
	return;
}

static ngx_int_t
ngx_http_vod_filter_init(void *data)
{
	ngx_child_request_context_t* ctx;
	ngx_http_request_t *r = data;
	ngx_http_upstream_t *u;

	u = r->upstream;

	if (u->headers_in.content_length_n + 1 > u->buffer.end - u->buffer.start) 
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_filter_init: content length %O exceeds buffer size %O", u->headers_in.content_length_n, (off_t)(u->buffer.end - u->buffer.start));
		return NGX_ERROR;
	}

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	u->buffer.last = u->buffer.start + ctx->read_body_size;
	u->length = u->headers_in.content_length_n;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_filter(void *data, ssize_t bytes)
{
	ngx_http_request_t *r = data;
	ngx_http_upstream_t *u;
	ngx_buf_t *b;

	u = r->upstream;
	b = &u->buffer;

	// only need to update the length and buffer position
	b->last += bytes;
	u->length -= bytes;

	return NGX_OK;
}

static ngx_int_t
ngx_create_upstream(
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
			"ngx_create_upstream: failed to create upstream rc=%i", rc);
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

static ngx_flag_t
ngx_should_proxy_header(ngx_table_elt_t* header, ngx_child_request_params_t* params)
{
	if (header->key.len == sizeof("host") - 1 &&
		ngx_memcmp(header->lowcase_key, "host", sizeof("host") - 1) == 0)
	{
		return 0;
	}

	if (!params->proxy_range &&
		header->key.len == sizeof("range") - 1 &&
		ngx_memcmp(header->lowcase_key, "range", sizeof("range") - 1) == 0)
	{
		return 0;
	}

	if (!params->proxy_accept_encoding &&
		header->key.len == sizeof("accept-encoding") - 1 &&
		ngx_memcmp(header->lowcase_key, "accept-encoding", sizeof("accept-encoding") - 1) == 0)
	{
		return 0;
	}

	return 1;
}

static ngx_buf_t*
ngx_init_request_buffer(
	ngx_http_request_t *r,
	ngx_buf_t* request_buffer,
	ngx_child_request_params_t* params)
{
	ngx_http_core_srv_conf_t *cscf;
	ngx_list_part_t              *part;
	ngx_table_elt_t              *header;
	ngx_uint_t                    i;
	ngx_flag_t range_request = params->range_start < params->range_end;
	ngx_buf_t *b;
	uintptr_t escape_chars;
	size_t len;
	u_char* p;

	// if the host name is empty, use by default the host name of the incoming request
	if (params->host_name.len == 0)
	{
		if (r->headers_in.host != NULL)
		{
			params->host_name = r->headers_in.host->value;
		}
		else
		{
			cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);
			params->host_name = cscf->server_name;
		}
	}

	// calculate the request size
	if (params->escape_uri)
	{
		escape_chars = ngx_escape_uri(NULL, params->base_uri.data, params->base_uri.len, NGX_ESCAPE_URI);
	}
	else
	{
		escape_chars = 0;
	}

	len =
		sizeof("HEAD ") - 1 + params->base_uri.len + 2 * escape_chars + sizeof("?") - 1 + params->extra_args.len + sizeof(" HTTP/1.1" CRLF) - 1 +
		sizeof("Host: ") - 1 + params->host_name.len + sizeof(CRLF) - 1 +
		params->extra_headers.len +
		sizeof(CRLF);
	if (range_request)
	{
		len += sizeof("Range: bytes=") - 1 + NGX_INT64_LEN + sizeof("-") - 1 + NGX_INT64_LEN + sizeof(CRLF) - 1;
	}

	// add all input headers except host and possibly range
	part = &r->headers_in.headers.part;
	header = part->elts;

	for (i = 0; /* void */; i++) {

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			header = part->elts;
			i = 0;
		}

		if (!ngx_should_proxy_header(&header[i], params))
		{
			continue;
		}

		len += header[i].key.len + sizeof(": ") - 1
			+ header[i].value.len + sizeof(CRLF) - 1;
	}

	// get/allocate the request buffer
	if (request_buffer != NULL && len <= (size_t)(request_buffer->end - request_buffer->start))
	{
		b = request_buffer;
		b->pos = b->start;
		p = b->start;
	}
	else
	{
		b = ngx_create_temp_buf(r->pool, len);
		if (b == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_init_request_buffer: ngx_create_temp_buf failed");
			return NULL;
		}
		p = b->last;
	}

	// first header line
	if (params->method == NGX_HTTP_HEAD)
	{
		*p++ = 'H';		*p++ = 'E';		*p++ = 'A';		*p++ = 'D';
	}
	else
	{
		*p++ = 'G';		*p++ = 'E';		*p++ = 'T';
	}
	*p++ = ' ';

	if (escape_chars > 0)
	{
		p = (u_char *)ngx_escape_uri(p, params->base_uri.data, params->base_uri.len, NGX_ESCAPE_URI);
	}
	else
	{
		p = ngx_copy(p, params->base_uri.data, params->base_uri.len);
	}

	if (params->extra_args.len > 0)
	{
		if (ngx_strchr(params->base_uri.data, '?'))
		{
			*p++ = '&';
		}
		else
		{
			*p++ = '?';
		}

		p = ngx_copy(p, params->extra_args.data, params->extra_args.len);
	}
	p = ngx_copy(p, " HTTP/1.1" CRLF, sizeof(" HTTP/1.1" CRLF) - 1);

	// host line
	p = ngx_copy(p, "Host: ", sizeof("Host: ") - 1);
	p = ngx_copy(p, params->host_name.data, params->host_name.len);
	*p++ = CR;	*p++ = LF;

	// range request
	if (range_request)
	{
		p = ngx_sprintf(p, "Range: bytes=%O-%O" CRLF, params->range_start, params->range_end - 1);
	}

	// additional headers
	p = ngx_copy(p, params->extra_headers.data, params->extra_headers.len);

	// input headers
	part = &r->headers_in.headers.part;
	header = part->elts;

	for (i = 0; /* void */; i++) {

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			header = part->elts;
			i = 0;
		}

		if (!ngx_should_proxy_header(&header[i], params))
		{
			continue;
		}

		p = ngx_copy(p, header[i].key.data, header[i].key.len);

		*p++ = ':'; *p++ = ' ';

		p = ngx_copy(p, header[i].value.data, header[i].value.len);

		*p++ = CR; *p++ = LF;
	}

	// headers end
	*p++ = CR;	*p++ = LF;
	*p = '\0';
	b->last = p;

	return b;
}

ngx_int_t
ngx_dump_request(
	ngx_http_request_t *r,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_child_request_params_t* params)
{
	ngx_child_request_base_context_t* ctx;
	ngx_int_t rc;

	// replace the module context - after calling ngx_dump_request the caller is not allowed to use the context
	ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
	if (ctx == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_dump_request: ngx_pcalloc failed");
		return NGX_ERROR;
	}
	ngx_http_set_ctx(r, ctx, ngx_http_vod_module);

	// create an upstream
	rc = ngx_create_upstream(r, upstream_conf);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_dump_request: ngx_create_upstream failed %i", rc);
		return rc;
	}

	// build the request
	ctx->request_buffer = ngx_init_request_buffer(r, NULL, params);
	if (ctx->request_buffer == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_dump_request: ngx_init_request_buffer failed");
		return NGX_ERROR;
	}

#if defined nginx_version && nginx_version >= 8011
	r->main->count++;
#endif
	
	// start the request
	ngx_http_upstream_init(r);

	return NGX_DONE;
}

static ngx_int_t
ngx_http_vod_upstream_non_buffered_filter_init(void *data)
{
	ngx_http_request_t *r = data;
	ngx_http_upstream_t *u;

	u = r->upstream;

	if (u->headers_in.content_length_n > 0)
	{
		u->length = u->headers_in.content_length_n;
	}

	return NGX_OK;
}

// Note: this function is a copy of ngx_http_upstream_non_buffered_filter, only reason to have
//		it here is in order to override input_filter_init to set the response length
//		(it is not possible to override only input_filter_init)
static ngx_int_t
ngx_http_vod_upstream_non_buffered_filter(void *data, ssize_t bytes)
{
	ngx_http_request_t  *r = data;

	ngx_buf_t            *b;
	ngx_chain_t          *cl, **ll;
	ngx_http_upstream_t  *u;

	u = r->upstream;

	for (cl = u->out_bufs, ll = &u->out_bufs; cl; cl = cl->next) {
		ll = &cl->next;
	}

	cl = ngx_chain_get_free_buf(r->pool, &u->free_bufs);
	if (cl == NULL) {
		return NGX_ERROR;
	}

	*ll = cl;

	cl->buf->flush = 1;
	cl->buf->memory = 1;

	b = &u->buffer;

	cl->buf->pos = b->last;
	b->last += bytes;
	cl->buf->last = b->last;
	cl->buf->tag = u->output.tag;

	if (u->length == -1) {
		return NGX_OK;
	}

	u->length -= bytes;

	return NGX_OK;
}


ngx_int_t
ngx_child_request_internal_handler(ngx_http_request_t *r)
{
	ngx_child_request_context_t* ctx;
	ngx_http_upstream_t *u;
	ngx_int_t rc;

	// create the upstream
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	rc = ngx_create_upstream(r, ctx->upstream_conf);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_internal_handler: ngx_create_upstream failed %i", rc);
		return rc;
	}

	u = r->upstream;

	if (ctx->in_memory)
	{
		// initialize the upstream buffer
		u->buffer.start = ctx->headers_buffer;
		u->buffer.pos = u->buffer.start;
		u->buffer.last = u->buffer.start;
		u->buffer.end = u->buffer.start + ctx->headers_buffer_size;
		u->buffer.temporary = 1;

		// create the headers list
		if (ngx_list_init(&u->headers_in.headers, r->pool, 8, sizeof(ngx_table_elt_t)) != NGX_OK)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_child_request_internal_handler: ngx_list_init failed");
			return NGX_ERROR;
		}

		// initialize the input filter
		u->input_filter_init = ngx_http_vod_filter_init;
		u->input_filter = ngx_http_vod_filter;
		u->input_filter_ctx = r;
	}
	else
	{
		// initialize the input filter
		u->input_filter_init = ngx_http_vod_upstream_non_buffered_filter_init;
		u->input_filter = ngx_http_vod_upstream_non_buffered_filter;
		u->input_filter_ctx = r;
	}

#if defined nginx_version && nginx_version >= 8011
	r->main->count++;
#endif

	// start the request
	ngx_http_upstream_init(r);

	return NGX_DONE;
}

static void
ngx_http_vod_wev_handler(ngx_http_request_t *r)
{
	ngx_child_request_context_t* ctx;
	ngx_http_upstream_t *u;
	ngx_int_t rc;
	off_t content_length;

	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	// restore the write event handler
	r->write_event_handler = ctx->original_write_event_handler;
	ctx->original_write_event_handler = NULL;

	// restore the original context
	ngx_http_set_ctx(r, ctx->original_context, ngx_http_vod_module);

	// get the completed upstream
	u = ctx->upstream;
	ctx->upstream = NULL;

	if (u == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_wev_handler: unexpected, upstream is null");
		return;
	}

	// code taken from echo-nginx-module to work around nginx subrequest issues
	if (r == r->connection->data && r->postponed) {

		if (r->postponed->request) {
			r->connection->data = r->postponed->request;

#if defined(nginx_version) && nginx_version >= 8012
			ngx_http_post_request(r->postponed->request, NULL);
#else
			ngx_http_post_request(r->postponed->request);
#endif

		}
		else {
			ngx_http_output_filter(r, NULL);
		}
	}

	// make sure all data was read
	rc = ctx->error_code;
	if (rc == NGX_OK && u->length != 0 && u->length != -1)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_wev_handler: upstream connection was closed with %O bytes left to read", u->length);
		rc = NGX_HTTP_BAD_GATEWAY;
	}
	
	// get the content length
	if (ctx->in_memory)
	{
		content_length = u->buffer.last - u->buffer.pos;
	}
	else if (u->state != NULL)
	{
		content_length = u->state->response_length;
	}
	else
	{
		content_length = 0;
	}

	// notify the caller
	ctx->callback(ctx->callback_context, rc, content_length, &u->buffer);
}

static ngx_int_t
ngx_child_request_finished_handler(
	ngx_http_request_t *r, 
	void *data, 
	ngx_int_t rc)
{
	ngx_http_request_t          *pr;
	ngx_child_request_context_t* ctx;

	// make sure we are not called twice for the same request
	r->post_subrequest = NULL;

	// save the completed upstream and error code in the context for the write event handler
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);

	ctx->upstream = r->upstream;
	ctx->error_code = rc;

	if (ctx->original_write_event_handler != NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_child_request_finished_handler: "
			"unexpected original_write_event_handler not null");
		return NGX_ERROR;
	}

	// replace the parent write event handler
	pr = r->parent;

	ctx->original_write_event_handler = pr->write_event_handler;
	pr->write_event_handler = ngx_http_vod_wev_handler;

	// temporarily replace the parent context
	ctx->original_context = ngx_http_get_module_ctx(pr, ngx_http_vod_module);
	ngx_http_set_ctx(pr, ctx, ngx_http_vod_module);

	// work-around issues in nginx's event module (from echo-nginx-module)
	if (r != r->connection->data
		&& r->postponed
		&& (r->main->posted_requests == NULL
		|| r->main->posted_requests->request != pr))
	{
#if defined(nginx_version) && nginx_version >= 8012
		ngx_http_post_request(pr, NULL);
#else
		ngx_http_post_request(pr);
#endif
	}

	return NGX_OK;
}

// Note: must not return positive error codes (like NGX_HTTP_INTERNAL_SERVER_ERROR)
ngx_int_t
ngx_child_request_start(
	ngx_http_request_t *r,
	ngx_child_request_buffers_t* buffers, 
	ngx_child_request_callback_t callback,
	void* callback_context,
	ngx_http_upstream_conf_t* upstream_conf,
	ngx_str_t* internal_location,
	ngx_child_request_params_t* params,
	off_t max_response_length,
	u_char* response_buffer)
{
	ngx_child_request_context_t* child_ctx;
	ngx_http_post_subrequest_t *psr;
	ngx_http_request_t *sr;
	ngx_uint_t flags;
	ngx_str_t args = ngx_null_string;
	ngx_str_t uri;
	ngx_int_t rc;
	u_char* p;

	// initialize the headers buffer
	if (buffers->headers_buffer == NULL || buffers->headers_buffer_size < upstream_conf->buffer_size)
	{
		if (buffers->headers_buffer != NULL)
		{
			ngx_pfree(r->pool, buffers->headers_buffer);
		}

		buffers->headers_buffer_size = upstream_conf->buffer_size;
		buffers->headers_buffer = ngx_palloc(r->pool, buffers->headers_buffer_size);
		if (buffers->headers_buffer == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_child_request_start: ngx_palloc failed (1)");
			return NGX_ERROR;
		}
	}

	// initialize the request buffer
	buffers->request_buffer = ngx_init_request_buffer(r, buffers->request_buffer, params);
	if (buffers->request_buffer == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_start: ngx_init_request_buffer failed");
		return NGX_ERROR;
	}

	// create the child context
	child_ctx = ngx_pcalloc(r->pool, sizeof(*child_ctx));
	if (child_ctx == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_start: ngx_pcalloc failed");
		return NGX_ERROR;
	}

	child_ctx->callback = callback;
	child_ctx->callback_context = callback_context;
	child_ctx->response_buffer_size = max_response_length;
	child_ctx->response_buffer = response_buffer;
	child_ctx->upstream_conf = upstream_conf;
	child_ctx->headers_buffer_size = buffers->headers_buffer_size;
	child_ctx->headers_buffer = buffers->headers_buffer;
	child_ctx->base.request_buffer = buffers->request_buffer;
	child_ctx->in_memory = max_response_length != 0;

	// build the subrequest uri
	// Note: this uri is not important, we could have just used internal_location as is
	//		but adding the child request uri makes the logs more readable
	uri.data = ngx_palloc(r->pool, internal_location->len + params->base_uri.len + 1);
	if (uri.data == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_start: ngx_palloc failed (2)");
		return NGX_ERROR;
	}
	p = ngx_copy(uri.data, internal_location->data, internal_location->len);
	p = ngx_copy(p, params->base_uri.data, params->base_uri.len);
	*p = '\0';
	uri.len = p - uri.data;

	// create the subrequest
	psr = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
	if (psr == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_start: ngx_palloc failed (3)");
		return NGX_ERROR;
	}

	psr->handler = ngx_child_request_finished_handler;
	psr->data = r;

	if (child_ctx->in_memory)
	{
		flags = NGX_HTTP_SUBREQUEST_WAITED | NGX_HTTP_SUBREQUEST_IN_MEMORY;
	}
	else
	{
		flags = NGX_HTTP_SUBREQUEST_WAITED;
	}

	rc = ngx_http_subrequest(r, &uri, &args, &sr, psr, flags);
	if (rc == NGX_ERROR) 
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_child_request_start: ngx_http_subrequest failed %i", rc);
		return rc;
	}

	// set the context of the subrequest
	ngx_http_set_ctx(sr, child_ctx, ngx_http_vod_module);

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		"ngx_child_request_start: completed successfully sr=%p", sr);

	return NGX_AGAIN;
}

void
ngx_child_request_free_buffers(ngx_pool_t* pool, ngx_child_request_buffers_t* buffers)
{
	ngx_pfree(pool, buffers->headers_buffer);
	buffers->headers_buffer = NULL;
	ngx_pfree(pool, buffers->request_buffer);
	buffers->request_buffer = NULL;
}

void 
ngx_init_upstream_conf(ngx_http_upstream_conf_t* upstream)
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
ngx_merge_upstream_conf(
	ngx_conf_t *cf,
	ngx_http_upstream_conf_t* conf_upstream, 
	ngx_http_upstream_conf_t* prev_upstream)
{
	ngx_hash_init_t hash;

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

char *
ngx_http_upstream_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_upstream_conf_t *upstream_conf = (ngx_http_upstream_conf_t*)((u_char*)conf + cmd->offset);
	ngx_str_t                       *value;
	ngx_url_t                        u;

	if (upstream_conf->upstream)
	{
		return "is duplicate";
	}

	value = cf->args->elts;

	ngx_memzero(&u, sizeof(ngx_url_t));

	u.url = value[1];
	u.no_resolve = 1;
	u.default_port = 80;

	upstream_conf->upstream = ngx_http_upstream_add(cf, &u, 0);
	if (upstream_conf->upstream == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
			"ngx_http_upstream_command: ngx_http_upstream_add failed");
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}
