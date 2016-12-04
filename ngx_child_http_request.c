#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include <ngx_event.h>

#include "ngx_child_http_request.h"
#include "ngx_http_vod_module.h"

// constants
#define RANGE_FORMAT "bytes=%O-%O"

// macros
#define is_in_memory(ctx) (ctx->response_buffer != NULL)

// typedefs
typedef struct {

	// fixed
	ngx_child_request_callback_t callback;
	void* callback_context;

	// deferred init
	ngx_buf_t* response_buffer;
	ngx_list_t upstream_headers;

	// temporary completion state
	ngx_http_upstream_t *upstream;
	ngx_int_t error_code;
	ngx_http_event_handler_pt original_write_event_handler;
	void *original_context;

	// misc
	ngx_flag_t dont_send_header;
	ngx_int_t send_header_result;

} ngx_child_request_context_t;

typedef struct {
	ngx_str_t name;
	off_t offset;
} ngx_child_request_hide_header_t;

// constants
static ngx_str_t ngx_http_vod_head_method = { 4, (u_char *) "HEAD " };

static ngx_str_t range_key = ngx_string("Range");
static u_char* range_lowcase_key = (u_char*)"range";
static ngx_uint_t range_hash =
	ngx_hash(ngx_hash(ngx_hash(ngx_hash('r', 'a'), 'n'), 'g'), 'e');

static ngx_child_request_hide_header_t hide_headers[] = {
	{ ngx_string("Accept"), 
#if (NGX_HTTP_HEADERS)	
		offsetof(ngx_http_headers_in_t, accept)
#else
		-1
#endif
	},
	{ ngx_string("Accept-Charset"), -1 },
	{ ngx_string("Accept-Datetime"), -1 },
	{ ngx_string("Accept-Encoding"), 
#if (NGX_HTTP_GZIP)
		offsetof(ngx_http_headers_in_t, accept_encoding)
#else
		-1
#endif
	},
	{ ngx_string("Accept-Language"), 
#if (NGX_HTTP_HEADERS)	
		offsetof(ngx_http_headers_in_t, accept_language)
#else
		-1
#endif
	},
	{ ngx_string("If-Match"), offsetof(ngx_http_headers_in_t, if_match) },
	{ ngx_string("If-Modified-Since"), offsetof(ngx_http_headers_in_t, if_modified_since) },
	{ ngx_string("If-None-Match"), offsetof(ngx_http_headers_in_t, if_none_match) },
	{ ngx_string("If-Range"), offsetof(ngx_http_headers_in_t, if_range) },
	{ ngx_string("If-Unmodified-Since"), offsetof(ngx_http_headers_in_t, if_unmodified_since) },
	{ ngx_null_string, -1 },
};

// globals
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_hash_t hide_headers_hash;

static void
ngx_child_request_wev_handler(ngx_http_request_t *r)
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
			"ngx_child_request_wev_handler: unexpected, upstream is null");
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

	// get the final error code
	rc = ctx->error_code;
	if (rc == NGX_OK && is_in_memory(ctx))
	{
		switch (u->headers_in.status_n)
		{
		case NGX_HTTP_OK:
		case NGX_HTTP_PARTIAL_CONTENT:
			if (u->length != 0 && u->length != -1 && !u->headers_in.chunked)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_child_request_wev_handler: upstream connection was closed with %O bytes left to read", u->length);
				rc = NGX_HTTP_BAD_GATEWAY;
			}
			break;

		case NGX_HTTP_RANGE_NOT_SATISFIABLE:
			// ignore this error, treat it like a successful read with empty body
			rc = NGX_OK;
			u->buffer.last = u->buffer.pos;
			break;

		default:
			if (u->headers_in.status_n != 0)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_child_request_wev_handler: upstream returned a bad status %ui", u->headers_in.status_n);
			}
			else
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_child_request_wev_handler: failed to get upstream status");
			}
			rc = NGX_HTTP_BAD_GATEWAY;
			break;
		}
	}
	else if (rc == NGX_ERROR)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_wev_handler: got error -1, changing to 502");
		rc = NGX_HTTP_BAD_GATEWAY;
	}

	if (ctx->send_header_result != NGX_OK)
	{
		rc = ctx->send_header_result;
	}

	// get the content length
	if (is_in_memory(ctx))
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

	if (ctx->callback != NULL)
	{
		// notify the caller
		ctx->callback(ctx->callback_context, rc, &u->buffer, content_length);
	}
	else
	{
		if (r->header_sent || ctx->dont_send_header)
		{
			// flush the buffer and close the request
			ngx_http_send_special(r, NGX_HTTP_LAST);
			ngx_http_finalize_request(r, NGX_OK);
		}
		else
		{
			// finalize the request
			ngx_http_finalize_request(r, rc);
		}
	}
}

static ngx_int_t
ngx_child_request_finished_handler(
	ngx_http_request_t *r, 
	void *data, 
	ngx_int_t rc)
{
	ngx_http_request_t          *pr;
	ngx_child_request_context_t* ctx;

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		"ngx_child_request_finished_handler: error code %ui", rc);

	// make sure we are not called twice for the same request
	r->post_subrequest = NULL;

	// save the completed upstream and error code in the context for the write event handler
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_child_request_finished_handler: unexpected, context is null");
		return NGX_ERROR;
	}

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
	pr->write_event_handler = ngx_child_request_wev_handler;

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

static void
ngx_child_request_initial_wev_handler(ngx_http_request_t *r)
{
	ngx_child_request_context_t* ctx;
	ngx_http_upstream_t* u;
	ngx_connection_t    *c;

	c = r->connection;

	// call the default request handler
	r->write_event_handler = ngx_http_handler;
	ngx_http_handler(r);

	// if request was destroyed ignore
	if (c->destroyed)
	{
		return;
	}

	// at this point the upstream should have been allocated by the proxy module
	u = r->upstream;
	if (u == NULL)
	{
		ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
			"ngx_child_request_initial_wev_handler: upstream is null");
		return;
	}

	// if the upstream module already started receiving, don't touch the buffer
	if (u->buffer.start != NULL)
	{
		ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
			"ngx_child_request_initial_wev_handler: upstream buffer was already allocated");
		return;
	}

	// initialize the upstream buffer
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL)
	{
		ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
			"ngx_child_request_initial_wev_handler: context is null");
		return;
	}
	u->buffer = *ctx->response_buffer;

	// initialize the headers list
	u->headers_in.headers = ctx->upstream_headers;
	u->headers_in.headers.last = &u->headers_in.headers.part;
}

static void
ngx_child_request_update_multi_header(
	ngx_array_t* arr, 
	ngx_table_elt_t* cur_value, 
	ngx_table_elt_t* new_value)
{
	ngx_table_elt_t** cur = arr->elts;
	ngx_table_elt_t** last = cur + arr->nelts;

	for (; cur < last; cur++)
	{
		if (*cur != cur_value)
		{
			continue;
		}

		*cur = new_value;
		break;
	}
}

static ngx_int_t
ngx_child_request_copy_headers(
	ngx_http_request_t* r,
	ngx_child_request_params_t* params,
	ngx_http_headers_in_t* dest,
	ngx_http_headers_in_t* src)
{
	ngx_child_request_hide_header_t *hide_header;
	ngx_http_core_main_conf_t  *cmcf;
	ngx_list_part_t *part;
	ngx_table_elt_t *output;
	ngx_table_elt_t *ch;
	ngx_table_elt_t *h;
	ngx_uint_t i = 0;
	ngx_http_header_t *hh;
	ngx_table_elt_t  **ph;
	ngx_uint_t count;
	ngx_int_t rc;

	// get the total header count
	count = 0;
	for (part = &src->headers.part; part; part = part->next)
	{
		count += part->nelts;
	}

	// allocate dest array
	rc = ngx_list_init(&dest->headers, r->pool, count + 2, sizeof(ngx_table_elt_t));
	if (rc != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_child_request_copy_headers: ngx_list_init failed");
		return NGX_ERROR;
	}

	output = dest->headers.last->elts;

	// copy relevant headers
	cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
	part = &src->headers.part;
	h = part->elts;

	for (i = 0; /* void */; i++)
	{
		if (i >= part->nelts)
		{
			if (part->next == NULL)
			{
				break;
			}

			part = part->next;
			h = part->elts;
			i = 0;
		}

		ch = h + i;

		// remove range if needed
		if (ch->hash == range_hash && !params->proxy_range && params->range_start >= params->range_end &&
			ch->key.len == range_key.len &&
			ngx_memcmp(ch->lowcase_key, range_lowcase_key, range_key.len) == 0)
		{
			dest->range = NULL;
			continue;
		}

		
		if (!params->proxy_all_headers)
		{
			// remove headers from the hide list
			hide_header = ngx_hash_find(&hide_headers_hash, ch->hash, ch->lowcase_key, ch->key.len);
			if (hide_header != NULL)
			{
				if (hide_header->offset >= 0)
				{
					*(ngx_table_elt_t**)((u_char*)dest + hide_header->offset) = NULL;
				}
				continue;
			}
		}

		// add the header to the output list
		*output = *ch;

		// update the header pointer, if exists
		hh = ngx_hash_find(&cmcf->headers_in_hash, ch->hash,
			ch->lowcase_key, ch->key.len);
		if (hh)
		{
			if ((ch->key.len == sizeof("cookie") - 1 &&
				ngx_memcmp(ch->lowcase_key, "cookie", sizeof("cookie") - 1) == 0) ||
				(ch->key.len == sizeof("x-forwarded-for") - 1 &&
				ngx_memcmp(ch->lowcase_key, "x-forwarded-for", sizeof("x-forwarded-for") - 1) == 0))
			{
				// multi header
				ngx_child_request_update_multi_header(
					(ngx_array_t*)((char *)dest + hh->offset),
					ch,
					output);
			}
			else
			{
				// single header
				ph = (ngx_table_elt_t **)((char *)dest + hh->offset);

				*ph = output;
			}
		}

		output++;
	}

	// add the extra header if needed
	if (params->extra_header.key.len != 0)
	{
		*output++ = params->extra_header;
	}

	// set the range if needed
	if (params->range_start < params->range_end)
	{
		if (dest->range == NULL)
		{
			h = output++;
			h->hash = range_hash;
			h->key = range_key;
			h->lowcase_key = range_lowcase_key;
			dest->range = h;
		}
		else
		{
			h = dest->range;
		}

		h->value.data = ngx_pnalloc(r->pool, sizeof(RANGE_FORMAT) + 2 * NGX_OFF_T_LEN);
		if (h->value.data == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_child_request_copy_headers: ngx_pnalloc failed");
			return NGX_ERROR;
		}

		h->value.len = ngx_sprintf(
			h->value.data,
			RANGE_FORMAT,
			params->range_start,
			params->range_end - 1) - h->value.data;
		h->value.data[h->value.len] = '\0';
	}

	// update the element count
	dest->headers.last->nelts = output - (ngx_table_elt_t*)dest->headers.last->elts;

	return NGX_OK;
}

ngx_int_t
ngx_child_request_start(
	ngx_http_request_t *r,
	ngx_child_request_callback_t callback,
	void* callback_context,
	ngx_str_t* internal_location,
	ngx_child_request_params_t* params,
	ngx_buf_t* response_buffer)
{
	ngx_child_request_context_t* child_ctx;
	ngx_http_post_subrequest_t *psr;
	ngx_http_request_t *sr;
	ngx_uint_t flags;
	ngx_str_t uri;
	ngx_int_t rc;
	u_char* p;

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
	child_ctx->response_buffer = response_buffer;

	// build the subrequest uri
	uri.data = ngx_pnalloc(r->pool, internal_location->len + params->base_uri.len + 1);
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

	if (is_in_memory(child_ctx))
	{
		if (ngx_list_init(&child_ctx->upstream_headers, r->pool, 8,
			sizeof(ngx_table_elt_t)) != NGX_OK)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_child_request_start: ngx_list_init failed");
			return NGX_ERROR;
		}

		flags = NGX_HTTP_SUBREQUEST_WAITED | NGX_HTTP_SUBREQUEST_IN_MEMORY;
	}
	else
	{
		flags = NGX_HTTP_SUBREQUEST_WAITED;
	}

	rc = ngx_http_subrequest(r, &uri, &params->extra_args, &sr, psr, flags);
	if (rc == NGX_ERROR)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_child_request_start: ngx_http_subrequest failed %i", rc);
		return rc;
	}

	// set the context of the subrequest
	ngx_http_set_ctx(sr, child_ctx, ngx_http_vod_module);

	// change the write_event_handler in order to inject the response buffer into the upstream 
	//	(this can be done only after the proxy module allocates the upstream)
	if (is_in_memory(child_ctx))
	{
		sr->write_event_handler = ngx_child_request_initial_wev_handler;
	}

	// Note: ngx_http_subrequest always sets the subrequest method to GET
	if (params->method == NGX_HTTP_HEAD)
	{
		sr->method = NGX_HTTP_HEAD;
		sr->method_name = ngx_http_vod_head_method;
	}
	
	// build the request headers
	rc = ngx_child_request_copy_headers(r, params, &sr->headers_in, &r->headers_in);
	if (rc != NGX_OK)
	{
		return rc;
	}

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		"ngx_child_request_start: completed successfully sr=%p", sr);

	return NGX_AGAIN;
}

static ngx_int_t
ngx_child_request_header_filter(ngx_http_request_t *r)
{
	ngx_child_request_context_t* ctx;
	ngx_http_request_t* pr = r->parent;

	// if the request is not a child of a vod request, ignore
	if (pr == NULL || pr->header_sent || ngx_http_get_module_ctx(pr, ngx_http_vod_module) == NULL)
	{
		return ngx_http_next_header_filter(r);
	}

	// if the request is not a vod request or it's in memory, ignore
	ctx = ngx_http_get_module_ctx(r, ngx_http_vod_module);
	if (ctx == NULL || is_in_memory(ctx))
	{
		return ngx_http_next_header_filter(r);
	}

	if (r->headers_out.status != 0)
	{
		// send the parent request headers
		pr->headers_out = r->headers_out;
		ctx->send_header_result = ngx_http_send_header(pr);
	}
	else
	{
		// no status code, this can happen in case the proxy module got an invalid status line
		//	and assumed it's HTTP/0.9, just don't send any header and close the connection when done
		ctx->dont_send_header = 1;
		pr->keepalive = 0;
	}

	return ngx_http_next_header_filter(r);
}

ngx_int_t
ngx_child_request_init(ngx_conf_t *cf)
{
	ngx_child_request_hide_header_t *h;
	ngx_array_t hide_headers_arr;
	ngx_hash_key_t  *hk;
	ngx_hash_init_t hash;

	// Note: need to install a header filter in order to support dumping requests -
	//	the headers of the parent request need to be sent before any body data is written
	ngx_http_next_header_filter = ngx_http_top_header_filter;
	ngx_http_top_header_filter = ngx_child_request_header_filter;

	// initialize hide_headers_hash
	if (ngx_array_init(
		&hide_headers_arr,
		cf->temp_pool, 
		sizeof(hide_headers) / sizeof(hide_headers[0]), 
		sizeof(ngx_hash_key_t)) != NGX_OK)
	{
		return NGX_ERROR;
	}

	for (h = hide_headers; h->name.len; h++)
	{
		hk = ngx_array_push(&hide_headers_arr);
		if (hk == NULL) 
		{
			return NGX_ERROR;
		}

		hk->key = h->name;
		hk->key_hash = ngx_hash_key_lc(h->name.data, h->name.len);
		hk->value = h;
	}

	hash.max_size = 512;
	hash.bucket_size = ngx_align(64, ngx_cacheline_size);
	hash.name = "vod_hide_headers_hash";

	hash.hash = &hide_headers_hash;
	hash.key = ngx_hash_key_lc;
	hash.pool = cf->pool;
	hash.temp_pool = NULL;

	if (ngx_hash_init(&hash, hide_headers_arr.elts, hide_headers_arr.nelts) != NGX_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}
