#include "ngx_http_vod_utils.h"

static const ngx_int_t error_map[VOD_ERROR_LAST - VOD_ERROR_FIRST] = {
	NGX_HTTP_NOT_FOUND,				// VOD_BAD_DATA
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_ALLOC_FAILED
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_UNEXPECTED
	NGX_HTTP_BAD_REQUEST,			// VOD_BAD_REQUEST
	NGX_HTTP_SERVICE_UNAVAILABLE,	// VOD_BAD_MAPPING
	NGX_HTTP_NOT_FOUND,				// VOD_EXPIRED
	NGX_HTTP_NOT_FOUND,				// VOD_NO_STREAMS
	NGX_HTTP_NOT_FOUND,				// VOD_EMPTY_MAPPING
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_NOT_FOUND (not expected to reach top level)
	NGX_HTTP_INTERNAL_SERVER_ERROR, // VOD_REDIRECT (not expected to reach top level)
};

static ngx_str_t error_codes[VOD_ERROR_LAST - VOD_ERROR_FIRST] = {
	ngx_string("BAD_DATA"),
	ngx_string("ALLOC_FAILED"),
	ngx_string("UNEXPECTED"),
	ngx_string("BAD_REQUEST"),
	ngx_string("BAD_MAPPING"),
	ngx_string("EXPIRED"),
	ngx_string("NO_STREAMS"),
	ngx_string("EMPTY_MAPPING"),
	ngx_string("UNEXPECTED"),
	ngx_string("UNEXPECTED"),
};

static ngx_uint_t ngx_http_vod_status_index;

static ngx_str_t empty_string = ngx_null_string;

void ngx_http_vod_set_status_index(ngx_uint_t index)
{
	ngx_http_vod_status_index = index;
}

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
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
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
ngx_http_vod_status_to_ngx_error(
	ngx_http_request_t* r, 
	vod_status_t rc)
{
	ngx_http_variable_value_t *vv;
	ngx_int_t index;

	if (rc < VOD_ERROR_FIRST || rc >= VOD_ERROR_LAST)
	{
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	index = rc - VOD_ERROR_FIRST;

	// update the status variable
	// Note: need to explicitly set the value (instead of calculating it in get_handler)
	//		so that it won't get lost in case of a redirect to an error page
	vv = &r->variables[ngx_http_vod_status_index];

	vv->valid = 1;
	vv->not_found = 0;
	vv->no_cacheable = 0;

	vv->data = error_codes[index].data;
	vv->len = error_codes[index].len;

	return error_map[index];
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

ngx_int_t
ngx_http_vod_get_base_url(
	ngx_http_request_t* r,
	ngx_http_complex_value_t* conf_base_url,
	ngx_str_t* file_uri,
	ngx_str_t* result)
{
	ngx_flag_t use_https;
	ngx_str_t base_url;
	ngx_str_t* host_name = NULL;
	size_t uri_path_len;
	size_t result_size;
	u_char* last_slash;
	u_char* p;

	if (conf_base_url != NULL)
	{
		if (ngx_http_complex_value(
			r,
			conf_base_url,
			&base_url) != NGX_OK)
		{
			return NGX_ERROR;
		}

		if (base_url.len == 0)
		{
			// conf base url evaluated to empty string, use relative URLs
			return NGX_OK;
		}

		if (base_url.data[base_url.len - 1] == '/')
		{
			file_uri = &empty_string;
		}

		result_size = base_url.len;
	}
	else
	{
		// when the request has no host header (HTTP 1.0), use relative URLs
		if (r->headers_in.host == NULL)
		{
			return NGX_OK;
		}

		host_name = &r->headers_in.host->value;

		result_size = sizeof("https://") - 1 + host_name->len;
	}

	if (file_uri->len)
	{
		last_slash = ngx_http_vod_memrchr(file_uri->data, '/', file_uri->len);
		if (last_slash == NULL)
		{
			vod_log_error(VOD_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_get_base_url: no slash found in uri %V", file_uri);
			return NGX_ERROR;
		}

		uri_path_len = last_slash + 1 - file_uri->data;
	}
	else
	{
		uri_path_len = 0;
	}

	// allocate the base url
	result_size += uri_path_len + sizeof("/");
	p = ngx_palloc(r->pool, result_size);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_get_base_url: ngx_palloc failed");
		return NGX_ERROR;
	}

	// build the url
	result->data = p;

	if (conf_base_url != NULL)
	{
		p = vod_copy(p, base_url.data, base_url.len);
	}
	else
	{
#if (NGX_HTTP_SSL)
		use_https = (r->connection->ssl != NULL);
#else
		use_https = 0;
#endif // NGX_HTTP_SSL

		if (use_https)
		{
			p = ngx_copy(p, "https://", sizeof("https://") - 1);
		}
		else
		{
			p = ngx_copy(p, "http://", sizeof("http://") - 1);
		}

		p = ngx_copy(p, host_name->data, host_name->len);
	}

	p = ngx_copy(p, file_uri->data, uri_path_len);
	*p = '\0';

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_get_base_url: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return NGX_ERROR;
	}

	return NGX_OK;
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
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
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

#if (nginx_version >= 1023000)
static ngx_table_elt_t *
ngx_http_vod_push_cache_control(ngx_http_request_t *r)
{
	ngx_table_elt_t  *cc;

	cc = r->headers_out.cache_control;

	if (cc == NULL) {

		cc = ngx_list_push(&r->headers_out.headers);
		if (cc == NULL) {
			return NULL;
		}

		r->headers_out.cache_control = cc;
		cc->next = NULL;

		cc->hash = 1;
		ngx_str_set(&cc->key, "Cache-Control");

	} else {
		for (cc = cc->next; cc; cc = cc->next) {
			cc->hash = 0;
		}

		cc = r->headers_out.cache_control;
		cc->next = NULL;
	}

	return cc;
}
#else
static ngx_table_elt_t *
ngx_http_vod_push_cache_control(ngx_http_request_t *r)
{
	ngx_uint_t        i;
	ngx_table_elt_t  *cc, **ccp;

	ccp = r->headers_out.cache_control.elts;

	if (ccp == NULL) {

		if (ngx_array_init(&r->headers_out.cache_control, r->pool,
			1, sizeof(ngx_table_elt_t *))
			!= NGX_OK)
		{
			return NULL;
		}

		ccp = ngx_array_push(&r->headers_out.cache_control);
		if (ccp == NULL) {
			return NULL;
		}

		cc = ngx_list_push(&r->headers_out.headers);
		if (cc == NULL) {
			return NULL;
		}

		cc->hash = 1;
		ngx_str_set(&cc->key, "Cache-Control");
		*ccp = cc;

	} else {
		for (i = 1; i < r->headers_out.cache_control.nelts; i++) {
			ccp[i]->hash = 0;
		}

		cc = ccp[0];
	}

	return cc;
}
#endif

// A run down version of ngx_http_set_expires
ngx_int_t
ngx_http_vod_set_expires(ngx_http_request_t *r, time_t expires_time)
{
	size_t            len;
	time_t            now, max_age;
	ngx_table_elt_t  *e, *cc;

	e = r->headers_out.expires;

	if (e == NULL) {

		e = ngx_list_push(&r->headers_out.headers);
		if (e == NULL) {
			return NGX_ERROR;
		}

		r->headers_out.expires = e;
#if (nginx_version >= 1023000)
		e->next = NULL;
#endif

		e->hash = 1;
		ngx_str_set(&e->key, "Expires");
	}

	len = sizeof("Mon, 28 Sep 1970 06:00:00 GMT");
	e->value.len = len - 1;

	cc = ngx_http_vod_push_cache_control(r);
	if (cc == NULL) {
		e->hash = 0;
		return NGX_ERROR;
	}

	e->value.data = ngx_pnalloc(r->pool, len);
	if (e->value.data == NULL) {
		e->hash = 0;
		cc->hash = 0;
		return NGX_ERROR;
	}

	if (expires_time == 0) {
		ngx_memcpy(e->value.data, ngx_cached_http_time.data,
			ngx_cached_http_time.len + 1);
		ngx_str_set(&cc->value, "max-age=0");
		return NGX_OK;
	}

	now = ngx_time();

	max_age = expires_time;
	expires_time += now;

	ngx_http_time(e->value.data, expires_time);

	if (max_age < 0) {
		ngx_str_set(&cc->value, "no-cache");
		return NGX_OK;
	}

	cc->value.data = ngx_pnalloc(r->pool,
		sizeof("max-age=") + NGX_TIME_T_LEN + 1);
	if (cc->value.data == NULL) {
		cc->hash = 0;
		return NGX_ERROR;
	}

	cc->value.len = ngx_sprintf(cc->value.data, "max-age=%T", max_age)
		- cc->value.data;

	return NGX_OK;
}
