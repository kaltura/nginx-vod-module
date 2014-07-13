#include "ngx_http_vod_request_parse.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_conf.h"

static ngx_int_t 
extract_uri_tokens(ngx_http_request_t* r, request_params_t* request_params, u_char** file_name)
{
	ngx_str_t stripped_uri;
	u_char* last_slash = NULL;
	u_char* copy_start = r->uri.data;
	u_char* cur_pos;
	u_char* end_pos = r->uri.data + r->uri.len;
	u_char* p;
	ngx_int_t value;
	enum {
		PARSE_INITIAL,
		PARSE_FOUND_SLASH,
		PARSE_CLIP_TO,
		PARSE_CLIP_FROM,
	} state = PARSE_INITIAL;
	ngx_http_vod_loc_conf_t   *conf;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_vod_module);

	stripped_uri.data = ngx_palloc(r->pool, r->uri.len + 1);
	if (stripped_uri.data == NULL)
	{
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	p = stripped_uri.data;

	request_params->clip_from = 0;
	request_params->clip_to = UINT_MAX;

	for (cur_pos = r->uri.data; cur_pos < end_pos; cur_pos++)
	{
		if (*cur_pos != '/')
		{
			continue;
		}

		switch (state)
		{
		case PARSE_INITIAL:
			state = PARSE_FOUND_SLASH;
			break;

		case PARSE_FOUND_SLASH:
			if (cur_pos == last_slash + 1 + conf->clip_to_param_name.len && 
				ngx_memcmp(last_slash + 1, conf->clip_to_param_name.data, conf->clip_to_param_name.len) == 0)
			{
				state = PARSE_CLIP_TO;
			}

			if (cur_pos == last_slash + 1 + conf->clip_from_param_name.len &&
				ngx_memcmp(last_slash + 1, conf->clip_from_param_name.data, conf->clip_from_param_name.len) == 0)
			{
				state = PARSE_CLIP_FROM;
			}
			break;

		case PARSE_CLIP_TO:
		case PARSE_CLIP_FROM:
			value = ngx_atoi(last_slash + 1, cur_pos - last_slash - 1);
			if (value < 0)
			{
				// ignore
				state = PARSE_FOUND_SLASH;
				break;
			}

			if (state == PARSE_CLIP_TO)
			{
				request_params->clip_to = value;
				p = ngx_copy(p, copy_start, last_slash - conf->clip_to_param_name.len - 1 - copy_start);
			}
			else
			{
				request_params->clip_from = value;
				p = ngx_copy(p, copy_start, last_slash - conf->clip_from_param_name.len - 1 - copy_start);
			}

			copy_start = cur_pos;

			state = PARSE_FOUND_SLASH;
			break;
		}

		last_slash = cur_pos;
	}

	if (last_slash == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"extract_uri_tokens: no slashes in uri");
		return NGX_HTTP_BAD_REQUEST;
	}

	if (request_params->clip_from >= request_params->clip_to)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"extract_uri_tokens: clip from %uD is larger than clip to %uD", request_params->clip_from, request_params->clip_to);
		return NGX_HTTP_BAD_REQUEST;
	}

	p = ngx_copy(p, copy_start, last_slash - copy_start);
	*p = '\0';

	stripped_uri.len = p - stripped_uri.data;

	*file_name = last_slash + 1;

	// set the stripped uri back on the request so that we can use functions like ngx_http_map_uri_to_path
	r->uri = stripped_uri;

	return NGX_OK;
}

static bool_t 
parse_required_tracks(ngx_http_request_t* r, u_char* start_pos, u_char* end_pos, bool_t has_stream_index, request_params_t* request_params)
{
	int stream_index;
	int media_type;
	u_char* next_pos;

	if (start_pos >= end_pos && !has_stream_index)
	{
		// no required tracks specified, default to the first audio & first video tracks
		request_params->required_tracks[MEDIA_TYPE_VIDEO] = 1;
		request_params->required_tracks[MEDIA_TYPE_AUDIO] = 1;
		return NGX_OK;
	}

	ngx_memzero(request_params->required_tracks, sizeof(request_params->required_tracks));

	for (;;)
	{
		// find the token end pos
		for (next_pos = start_pos; next_pos < end_pos && *next_pos != '-'; next_pos++);

		if (has_stream_index)
		{
			// segment index
			has_stream_index = FALSE;

			request_params->segment_index = ngx_atoi(start_pos, next_pos - start_pos) - 1;		// convert to 0-based
			if (request_params->segment_index < 0)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"parse_required_tracks: failed to parse the segment index");
				return NGX_HTTP_BAD_REQUEST;
			}
		}
		else
		{
			// stream
			switch (*start_pos)
			{
			case 'v':
				media_type = MEDIA_TYPE_VIDEO;
				break;

			case 'a':
				media_type = MEDIA_TYPE_AUDIO;
				break;

			default:
				return NGX_HTTP_BAD_REQUEST;
			}

			start_pos++;		// skip the v/a
			stream_index = ngx_atoi(start_pos, next_pos - start_pos) - 1;		// convert to 0-based
			if (stream_index < 0)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"parse_required_tracks: failed to parse a stream index");
				return NGX_HTTP_BAD_REQUEST;
			}

			request_params->required_tracks[media_type] |= (1 << stream_index);
		}

		if (next_pos >= end_pos)
		{
			break;
		}
		start_pos = next_pos + 1;
	}

	if (request_params->required_tracks[MEDIA_TYPE_VIDEO] == 0 && request_params->required_tracks[MEDIA_TYPE_AUDIO] == 0)
	{
		// no required tracks specified, default to the first audio & first video tracks
		request_params->required_tracks[MEDIA_TYPE_VIDEO] = 1;
		request_params->required_tracks[MEDIA_TYPE_AUDIO] = 1;
	}

	return NGX_OK;
}

ngx_int_t 
parse_request_uri(ngx_http_request_t *r, ngx_http_vod_loc_conf_t *conf, request_params_t* request_params)
{
	ngx_int_t rc;
	u_char* start_pos = NULL;
	u_char* end_pos = r->uri.data + r->uri.len;

	rc = extract_uri_tokens(r, request_params, &start_pos);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (end_pos - start_pos >= (int)(conf->m3u8_config.segment_file_name_prefix.len + sizeof("1.ts") - 1) &&
		ngx_memcmp(end_pos - (sizeof(".ts") - 1), ".ts", sizeof(".ts") - 1) == 0)
	{
		// make sure the file name starts with the right prefix
		if (ngx_memcmp(start_pos, conf->m3u8_config.segment_file_name_prefix.data, conf->m3u8_config.segment_file_name_prefix.len) != 0)
		{
			return NGX_HTTP_BAD_REQUEST;
		}
		start_pos += conf->m3u8_config.segment_file_name_prefix.len;

		// parse the required tracks string
		rc = parse_required_tracks(r, start_pos, end_pos - (sizeof(".ts") - 1), TRUE, request_params);
		if (rc != NGX_OK)
		{
			return rc;
		}

		return NGX_OK;
	}
	
	if (end_pos - start_pos >= (int)(sizeof(".m3u8") - 1) &&
		ngx_memcmp(end_pos - (sizeof(".m3u8") - 1), ".m3u8", sizeof(".m3u8") - 1) == 0)
	{
		// make sure the file name begins with 'index' or 'iframes'
		if (end_pos - start_pos >= (int)(conf->index_file_name_prefix.len + sizeof(".m3u8") - 1) &&
			ngx_memcmp(start_pos, conf->index_file_name_prefix.data, conf->index_file_name_prefix.len) == 0)
		{
			request_params->segment_index = REQUEST_TYPE_PLAYLIST;
			start_pos += conf->index_file_name_prefix.len;
		}
		else if (end_pos - start_pos >= (int)(conf->iframes_file_name_prefix.len + sizeof(".m3u8") - 1) &&
			ngx_memcmp(start_pos, conf->iframes_file_name_prefix.data, conf->iframes_file_name_prefix.len) == 0)
		{
			request_params->segment_index = REQUEST_TYPE_IFRAME_PLAYLIST;
			start_pos += conf->iframes_file_name_prefix.len;
		}
		else
		{
			return NGX_HTTP_BAD_REQUEST;
		}

		if (*start_pos == '-')
		{
			start_pos++;
		}

		// parse the required tracks string
		rc = parse_required_tracks(r, start_pos, end_pos - (sizeof(".m3u8") - 1), FALSE, request_params);
		if (rc != NGX_OK)
		{
			return rc;
		}

		return NGX_OK;
	}
	
	if (conf->secret_key.len > 0 &&
		end_pos - start_pos == (int)conf->encryption_key_file_name.len &&
		ngx_memcmp(start_pos, conf->encryption_key_file_name.data, conf->encryption_key_file_name.len) == 0)
	{
		request_params->segment_index = REQUEST_TYPE_ENCRYPTION_KEY;
		
		return NGX_OK;
	}

	return NGX_HTTP_BAD_REQUEST;
}
