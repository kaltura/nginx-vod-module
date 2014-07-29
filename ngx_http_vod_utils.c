#include "ngx_http_vod_utils.h"

static const ngx_int_t error_map[VOD_ERROR_LAST - VOD_ERROR_FIRST] = {
	NGX_HTTP_NOT_FOUND,				// VOD_BAD_DATA
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_ALLOC_FAILED,
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_UNEXPECTED,
	NGX_HTTP_BAD_REQUEST,			// VOD_BAD_REQUEST,
};

ngx_int_t
send_single_buffer_response(ngx_http_request_t *r, ngx_str_t *response, u_char* content_type, size_t content_type_len)
{
	ngx_chain_t  out;
	ngx_int_t    rc;
	ngx_buf_t* b;

	// adjust the buffer flags
	b = ngx_pcalloc(r->pool, sizeof(*b));
	if (b == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"send_single_buffer_response: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b->pos = response->data;
	b->last = response->data + response->len;
	b->memory = 1;    // this buffer is in memory
	b->last_buf = 1;  // this is the last buffer in the buffer chain

	// attach the buffer to the chain
	out.buf = b;
	out.next = NULL;

	// set the content type
	r->headers_out.content_type_len = content_type_len;
	r->headers_out.content_type.len = content_type_len;
	r->headers_out.content_type.data = (u_char *)content_type;

	// set the status line
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = b->last - b->pos;

	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"send_single_buffer_response: ngx_http_set_etag failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	// send the headers
	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"send_single_buffer_response: ngx_http_send_header failed %i", rc);
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return rc;
	}

	// send the buffer chain
	rc = ngx_http_output_filter(r, &out);
	if (rc != NGX_OK && rc != NGX_AGAIN)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, state->log, 0,
			"send_single_buffer_response: ngx_http_output_filter failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t 
vod_status_to_ngx_error(vod_status_t rc)
{
	if (rc >= VOD_ERROR_FIRST && rc < VOD_ERROR_LAST)
	{
		return error_map[rc - VOD_ERROR_FIRST];
	}

	return NGX_HTTP_INTERNAL_SERVER_ERROR;
}

ngx_flag_t
header_exists(ngx_http_request_t* r, ngx_str_t* searched_header)
{
	ngx_table_elt_t *header;
	ngx_table_elt_t *last_header;
	ngx_list_part_t *part;

	part = &r->headers_in.headers.part;

	while (part)
	{
		header = part->elts;
		last_header = header + part->nelts;
		for (; header < last_header; header++)
		{
			if (header->key.len == searched_header->len &&
				ngx_strncasecmp(header->key.data, searched_header->data, searched_header->len) == 0)
			{
				return 1;
			}
		}
		part = part->next;
	}
	return 0;
}
