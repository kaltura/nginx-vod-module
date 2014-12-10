#include "ngx_http_vod_utils.h"

static const ngx_int_t error_map[VOD_ERROR_LAST - VOD_ERROR_FIRST] = {
	NGX_HTTP_NOT_FOUND,				// VOD_BAD_DATA
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_ALLOC_FAILED,
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_UNEXPECTED,
	NGX_HTTP_BAD_REQUEST,			// VOD_BAD_REQUEST,
};

ngx_int_t
ngx_http_vod_send_response(ngx_http_request_t *r, ngx_str_t *response, u_char* content_type, size_t content_type_len)
{
	ngx_chain_t  out;
	ngx_int_t    rc;
	ngx_buf_t* b;

	// set the content type
	r->headers_out.content_type_len = content_type_len;
	r->headers_out.content_type.len = content_type_len;
	r->headers_out.content_type.data = (u_char *)content_type;

	// set the status line
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = response->len;

	rc = ngx_http_set_etag(r);
	if (rc != NGX_OK) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_response: ngx_http_set_etag failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	// send the headers
	rc = ngx_http_send_header(r);
	if (rc == NGX_ERROR || rc > NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_response: ngx_http_send_header failed %i", rc);
		return rc;
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return rc;
	}

	// wrap the response with ngx_buf_t
	b = ngx_pcalloc(r->pool, sizeof(*b));
	if (b == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_response: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b->pos = response->data;
	b->last = response->data + response->len;
	if (response->len > 0)
	{
		b->memory = 1;    // this buffer is in memory
	}
	b->last_buf = 1;  // this is the last buffer in the buffer chain

	// attach the buffer to the chain
	out.buf = b;
	out.next = NULL;

	// send the buffer chain
	rc = ngx_http_output_filter(r, &out);
	if (rc != NGX_OK && rc != NGX_AGAIN)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_send_response: ngx_http_output_filter failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t 
ngx_http_vod_status_to_ngx_error(vod_status_t rc)
{
	if (rc >= VOD_ERROR_FIRST && rc < VOD_ERROR_LAST)
	{
		return error_map[rc - VOD_ERROR_FIRST];
	}

	return NGX_HTTP_INTERNAL_SERVER_ERROR;
}

ngx_flag_t
ngx_http_vod_header_exists(ngx_http_request_t* r, ngx_str_t* searched_header)
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

void
ngx_http_vod_get_base_url(
	ngx_http_request_t* r,
	ngx_http_vod_loc_conf_t* conf,
	ngx_str_t* file_uri,
	ngx_str_t* base_url)
{
	ngx_flag_t use_https;
	ngx_str_t* host_name;
	size_t uri_path_len;
	size_t result_size;
	u_char* last_slash;
	u_char* p;

	// when the request has no host header (HTTP 1.0), use relative URLs
	if (r->headers_in.host == NULL)
	{
		return;
	}

	host_name = &r->headers_in.host->value;

	if (file_uri->len)
	{
		last_slash = memrchr(file_uri->data, '/', file_uri->len);
		if (last_slash == NULL)
		{
			return;
		}

		uri_path_len = last_slash + 1 - file_uri->data;
	}
	else
	{
		uri_path_len = 0;
	}

	// allocate the base url
	result_size = sizeof("https://") - 1 + host_name->len + uri_path_len + sizeof("/");
	base_url->data = ngx_palloc(r->pool, result_size);
	if (base_url->data == NULL)
	{
		return;
	}

	// decide whether to use http or https
	if (conf->https_header_name.len)
	{
		use_https = ngx_http_vod_header_exists(r, &conf->https_header_name);
	}
	else
	{
#if (NGX_HTTP_SSL)
		use_https = (r->connection->ssl != NULL);
#else
		use_https = 0;
#endif
	}

	// build the url
	if (use_https)
	{
		p = ngx_copy(base_url->data, "https://", sizeof("https://") - 1);
	}
	else
	{
		p = ngx_copy(base_url->data, "http://", sizeof("http://") - 1);
	}

	p = ngx_copy(p, host_name->data, host_name->len);
	p = ngx_copy(p, file_uri->data, uri_path_len);
	*p = '\0';

	base_url->len = p - base_url->data;

	if (base_url->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_get_base_url: result length %uz exceeded allocated length %uz",
			base_url->len, result_size);
	}
}

ngx_int_t
ngx_http_vod_merge_string_parts(ngx_http_request_t* r, ngx_str_t* parts, uint32_t part_count, ngx_str_t* result)
{
	ngx_str_t* cur_part;
	ngx_str_t* last_part = parts + part_count;
	u_char* p;
	size_t len = 0;

	for (cur_part = parts; cur_part < last_part; cur_part++)
	{
		len += cur_part->len;
	}

	p = ngx_palloc(r->pool, len);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_merge_string_parts: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	result->data = p;

	for (cur_part = parts; cur_part < last_part; cur_part++)
	{
		p = ngx_copy(p, cur_part->data, cur_part->len);
	}

	result->len = p - result->data;

	return NGX_OK;
}
