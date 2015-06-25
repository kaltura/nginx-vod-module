#include "ngx_http_vod_utils.h"

static const ngx_int_t error_map[VOD_ERROR_LAST - VOD_ERROR_FIRST] = {
	NGX_HTTP_NOT_FOUND,				// VOD_BAD_DATA
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_ALLOC_FAILED,
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_UNEXPECTED,
	NGX_HTTP_BAD_REQUEST,			// VOD_BAD_REQUEST,
};

ngx_int_t
ngx_http_vod_send_response(ngx_http_request_t *r, ngx_str_t *response, ngx_str_t* content_type)
{
	ngx_chain_t  out;
	ngx_int_t    rc;
	ngx_buf_t* b;

	if (!r->header_sent)
	{
		// set the content type
		r->headers_out.content_type = *content_type;
		r->headers_out.content_type_len = content_type->len;

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
	}

	if (r->header_only || r->method == NGX_HTTP_HEAD)
	{
		return NGX_OK;
	}

	// wrap the response with ngx_buf_t
	b = ngx_calloc_buf(r->pool);
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
		b->temporary = 1;
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

static void *
ngx_http_vod_memrchr(const u_char *s, int c, size_t n)
{
	const u_char *cp;

	for (cp = s + n; cp > s; )
	{
		if (*(--cp) == (u_char)c)
			return (void*)cp;
	}
	return NULL;
}

void
ngx_http_vod_get_base_url(
	ngx_http_request_t* r,
	ngx_str_t* https_header_name,
	ngx_str_t* conf_base_url,
	ngx_flag_t conf_base_url_has_schema,
	ngx_str_t* file_uri,
	ngx_str_t* base_url)
{
	ngx_flag_t use_https;
	ngx_str_t* host_name;
	size_t uri_path_len;
	size_t result_size;
	u_char* last_slash;
	u_char* p;

	if (conf_base_url == NULL || conf_base_url->len == 0)
	{
		// when the request has no host header (HTTP 1.0), use relative URLs
		if (r->headers_in.host == NULL)
		{
			return;
		}

		host_name = &r->headers_in.host->value;
	}
	else
	{
		host_name = conf_base_url;
	}

	if (file_uri->len)
	{
		last_slash = ngx_http_vod_memrchr(file_uri->data, '/', file_uri->len);
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
	p = ngx_palloc(r->pool, result_size);
	if (p == NULL)
	{
		return;
	}

	// build the url
	base_url->data = p;

	if (!conf_base_url_has_schema)
	{
		// decide whether to use http or https
		if (https_header_name->len)
		{
			use_https = ngx_http_vod_header_exists(r, https_header_name);
		}
		else
		{
#if (NGX_HTTP_SSL)
			use_https = (r->connection->ssl != NULL);
#else
			use_https = 0;
#endif
		}

		if (use_https)
		{
			p = ngx_copy(p, "https://", sizeof("https://") - 1);
		}
		else
		{
			p = ngx_copy(p, "http://", sizeof("http://") - 1);
		}
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

// Implemented according to nginx's ngx_http_range_parse, dropping multi range support
ngx_int_t
ngx_http_vod_range_parse(ngx_str_t* range, off_t content_length, off_t* out_start, off_t* out_end)
{
    u_char            *p;
    off_t              start, end, cutoff, cutlim;
    ngx_uint_t         suffix;

    if (range->len < 7 ||
        ngx_strncasecmp(range->data,
        (u_char *) "bytes=", 6) != 0) {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }

    p = range->data + 6;

    cutoff = NGX_MAX_OFF_T_VALUE / 10;
    cutlim = NGX_MAX_OFF_T_VALUE % 10;

    start = 0;
    end = 0;
    suffix = 0;

    while (*p == ' ') { p++; }

    if (*p != '-') {
        if (*p < '0' || *p > '9') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        while (*p >= '0' && *p <= '9') {
            if (start >= cutoff && (start > cutoff || *p - '0' > cutlim)) {
                return NGX_HTTP_RANGE_NOT_SATISFIABLE;
            }

            start = start * 10 + *p++ - '0';
        }

        while (*p == ' ') { p++; }

        if (*p++ != '-') {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        while (*p == ' ') { p++; }

        if (*p == '\0') {
            end = content_length;
            goto found;
        }

    } else {
        suffix = 1;
        p++;
    }

    if (*p < '0' || *p > '9') {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }

    while (*p >= '0' && *p <= '9') {
        if (end >= cutoff && (end > cutoff || *p - '0' > cutlim)) {
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }

        end = end * 10 + *p++ - '0';
    }

    while (*p == ' ') { p++; }

    if (*p != '\0') {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }

    if (suffix) {
        start = content_length - end;
        end = content_length - 1;
    }

    if (end >= content_length) {
        end = content_length;

    } else {
        end++;
    }

found:

    if (start >= end) {
        return NGX_HTTP_RANGE_NOT_SATISFIABLE;
    }

    *out_start = start;
    *out_end = end;

    return NGX_OK;
}
