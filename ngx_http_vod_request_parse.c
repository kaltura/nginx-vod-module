#include "ngx_http_vod_request_parse.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_conf.h"
#include "ngx_http_vod_utils.h"

// typedefs
typedef ngx_int_t(*ngx_http_vod_param_parser_t)(ngx_str_t* value, ngx_http_vod_suburi_params_t* output, int offset);

typedef struct {
	int name_conf_offset;
	ngx_http_vod_param_parser_t parser;
	int target_offset;
} ngx_http_vod_uri_param_def_t;

typedef struct {
	ngx_str_t prefix;
	ngx_str_t middle_parts[MAX_SUB_URIS];
	ngx_str_t postfix;
	uint32_t parts_count;
} ngx_http_vod_multi_uri_t;

bool_t
ngx_http_vod_parse_string(
	const ngx_http_vod_match_definition_t* match_def, 
	u_char* start_pos, 
	u_char* end_pos, 
	void* output)
{
	uint64_t value;
	u_char* delim_pos;

	for (;;)
	{
		switch (match_def->match_type)
		{
		case MATCH_END:
			return start_pos == end_pos;

		case MATCH_FIXED_STRING:
			if (end_pos - start_pos < (ssize_t)match_def->string.len ||
				ngx_memcmp(start_pos, match_def->string.data, match_def->string.len) != 0)
			{
				return FALSE;
			}

			start_pos += match_def->string.len;
			break;

		case MATCH_NUMBER:
			value = 0;
			for (; start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9'; start_pos++)
			{
				value = value * 10 + *start_pos - '0';
			}
			*(uint64_t*)((u_char*)output + match_def->target_offset) = value;
			break;

		case MATCH_DELIM_STRING:
			delim_pos = memchr(start_pos, match_def->delim, end_pos - start_pos);
			if (delim_pos == NULL)
			{
				return FALSE;
			}

			((ngx_str_t*)((u_char*)output + match_def->target_offset))->data = start_pos;
			((ngx_str_t*)((u_char*)output + match_def->target_offset))->len = delim_pos - start_pos;
			start_pos = delim_pos + 1;
			break;
		}
		match_def++;
	}
}

bool_t
ngx_http_vod_split_uri_file_name(
	ngx_str_t* uri,
	int components,
	ngx_str_t* path,
	ngx_str_t* file_name)
{
	u_char* cur_pos = uri->data + uri->len - 1;

	for (cur_pos = uri->data + uri->len - 1; cur_pos >= uri->data; cur_pos--)
	{
		if (*cur_pos != '/')
		{
			continue;
		}

		components--;
		if (components > 0)
		{
			continue;
		}

		path->data = uri->data;
		path->len = cur_pos - uri->data;
		file_name->data = cur_pos + 1;
		file_name->len = uri->data + uri->len - file_name->data;
		return TRUE;
	}

	return FALSE;
}

static u_char*
ngx_http_vod_extract_uint32_token(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t value = 0;
	for (; start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9'; start_pos++)
	{
		value = value * 10 + *start_pos - '0';
	}
	*result = value;
	return start_pos;
}

u_char*
ngx_http_vod_extract_uint32_token_reverse(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t multiplier;
	uint32_t value = 0;
	for (multiplier = 1;
		end_pos > start_pos && end_pos[-1] >= '0' && end_pos[-1] <= '9';
		end_pos--, multiplier *= 10)
	{
		value += (end_pos[-1] - '0') * multiplier;
	}
	*result = value;
	return end_pos;
}

static u_char*
ngx_http_vod_extract_track_tokens(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t stream_index;
	u_char* next_pos;
	int media_type;

	// by default use the first audio and first video streams
	if (start_pos >= end_pos || (*start_pos != 'a' && *start_pos != 'v'))
	{
		result[MEDIA_TYPE_VIDEO] = 1;
		result[MEDIA_TYPE_AUDIO] = 1;
		return start_pos;
	}

	while (start_pos < end_pos)
	{
		switch (*start_pos)
		{
		case 'v':
			media_type = MEDIA_TYPE_VIDEO;
			break;

		case 'a':
			media_type = MEDIA_TYPE_AUDIO;
			break;

		default:
			return start_pos;
		}

		start_pos++;		// skip the v/a

		next_pos = ngx_http_vod_extract_uint32_token(start_pos, end_pos, &stream_index);

		if (stream_index == 0)
		{
			// no index => all streams of the media type
			result[media_type] = 0xffffffff;
		}
		else
		{
			result[media_type] |= (1 << (stream_index - 1));
		}

		start_pos = next_pos;

		if (start_pos < end_pos && *start_pos == '-')
		{
			start_pos++;
		}
	}
	return start_pos;
}

static u_char*
ngx_http_vod_extract_file_tokens(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t file_index;
	u_char* next_pos;

	// by default use all files
	if (start_pos >= end_pos || *start_pos != 'f')
	{
		*result = 0xffffffff;
		return start_pos;
	}

	while (start_pos < end_pos)
	{
		if (*start_pos != 'f')
		{
			return start_pos;
		}

		start_pos++;		// skip the f

		next_pos = ngx_http_vod_extract_uint32_token(start_pos, end_pos, &file_index);

		if (file_index == 0)
		{
			// no index => all files
			*result = 0xffffffff;
		}
		else
		{
			*result |= (1 << (file_index - 1));
		}

		start_pos = next_pos;

		if (start_pos < end_pos && *start_pos == '-')
		{
			start_pos++;
		}
	}
	return start_pos;
}

ngx_int_t
ngx_http_vod_parse_uri_file_name(
	ngx_http_request_t* r,
	u_char* start_pos,
	u_char* end_pos,
	bool_t expect_segment_index,
	ngx_http_vod_request_params_t* result)
{
	if (start_pos < end_pos && *start_pos == '-')
	{
		start_pos++;
	}

	if (expect_segment_index)
	{
		start_pos = ngx_http_vod_extract_uint32_token(start_pos, end_pos, &result->segment_index);
		if (result->segment_index <= 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri_file_name: ngx_http_vod_extract_uint32_token failed");
			return NGX_HTTP_BAD_REQUEST;
		}
		result->segment_index--;		// convert to 0-based

		if (start_pos < end_pos && *start_pos == '-')
		{
			start_pos++;
		}
	}

	start_pos = ngx_http_vod_extract_file_tokens(start_pos, end_pos, &result->required_files);

	start_pos = ngx_http_vod_extract_track_tokens(start_pos, end_pos, result->required_tracks);

	if (start_pos != end_pos)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_file_name: did not consume the whole name");
		return NGX_HTTP_BAD_REQUEST;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_parse_multi_uri(
	ngx_http_request_t* r,
	ngx_str_t* uri,
	ngx_str_t* multi_uri_suffix,
	ngx_http_vod_multi_uri_t* result)
{
	u_char* cur_pos;
	u_char* end_pos;
	u_char* last_comma_pos;
	uint32_t part_index;

	result->prefix.data = uri->data;
	result->prefix.len = uri->len;

	if (uri->len < multi_uri_suffix->len ||
		ngx_memcmp(multi_uri_suffix->data, uri->data + uri->len - multi_uri_suffix->len, multi_uri_suffix->len) != 0)
	{
		// not a multi uri
		result->postfix.data = NULL;
		result->postfix.len = 0;
		result->middle_parts[0].data = NULL;
		result->middle_parts[0].len = 0;
		result->parts_count = 1;
		return NGX_OK;
	}

	uri->len -= multi_uri_suffix->len;

	end_pos = uri->data + uri->len;
	last_comma_pos = NULL;
	part_index = 0;
	for (cur_pos = uri->data; cur_pos < end_pos; cur_pos++)
	{
		if (*cur_pos != ',')
		{
			continue;
		}

		if (last_comma_pos == NULL)
		{
			result->prefix.len = cur_pos - uri->data;
		}
		else
		{
			if (part_index >= MAX_SUB_URIS)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_parse_multi_uri: number of url parts exceeds the limit");
				return NGX_HTTP_BAD_REQUEST;
			}

			result->middle_parts[part_index].data = last_comma_pos;
			result->middle_parts[part_index].len = cur_pos - last_comma_pos;
			part_index++;
		}

		last_comma_pos = cur_pos + 1;
	}

	if (last_comma_pos == NULL)
	{
		// no commas at all
		result->postfix.data = NULL;
		result->postfix.len = 0;
	}
	else
	{
		// 1 comma or more
		result->postfix.data = last_comma_pos;
		result->postfix.len = end_pos - last_comma_pos;
	}

	if (part_index == 0)
	{
		// no commas at all or a single comma
		result->middle_parts[0].data = NULL;
		result->middle_parts[0].len = 0;
		result->parts_count = 1;
	}
	else
	{
		// 2 commas or more
		result->parts_count = part_index;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_parse_uint32_param(ngx_str_t* value, ngx_http_vod_suburi_params_t* output, int offset)
{
	ngx_int_t result;

	result = ngx_atoi(value->data, value->len);
	if (result < 0)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	*(uint32_t*)((u_char*)output + offset) = result;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_parse_tracks_param(ngx_str_t* value, ngx_http_vod_suburi_params_t* output, int offset)
{
	u_char* end_pos;

	output->required_tracks[MEDIA_TYPE_AUDIO] = 0;
	output->required_tracks[MEDIA_TYPE_VIDEO] = 0;
	end_pos = ngx_http_vod_extract_track_tokens(value->data, value->data + value->len, output->required_tracks);
	if (end_pos != value->data + value->len)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_parse_speed_param(ngx_str_t* value, ngx_http_vod_suburi_params_t* output, int offset)
{
	ngx_int_t result;

	result = ngx_atofp(value->data, value->len, 1);
	if (result < 0)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	if (result < 5 || result > 20)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	output->speed_nom = result;
	output->speed_denom = 10;

	return NGX_OK;
}

static ngx_http_vod_uri_param_def_t uri_param_defs[] = {
	{ offsetof(ngx_http_vod_loc_conf_t, clip_to_param_name), ngx_http_vod_parse_uint32_param, offsetof(ngx_http_vod_suburi_params_t, clip_to) },
	{ offsetof(ngx_http_vod_loc_conf_t, clip_from_param_name), ngx_http_vod_parse_uint32_param, offsetof(ngx_http_vod_suburi_params_t, clip_from) },
	{ offsetof(ngx_http_vod_loc_conf_t, tracks_param_name), ngx_http_vod_parse_tracks_param, 0 },
	{ offsetof(ngx_http_vod_loc_conf_t, speed_param_name), ngx_http_vod_parse_speed_param, 0 },
};

ngx_int_t
ngx_http_vod_init_uri_params_hash(ngx_conf_t *cf, ngx_http_vod_loc_conf_t* conf)
{
	ngx_hash_key_t hash_keys[sizeof(uri_param_defs) / sizeof(uri_param_defs[0])];
	ngx_hash_init_t hash;
	ngx_str_t* param_name;
	ngx_int_t rc;
	unsigned i;

	for (i = 0; i < sizeof(hash_keys) / sizeof(hash_keys[0]); i++)
	{
		param_name = (ngx_str_t*)((u_char*)conf + uri_param_defs[i].name_conf_offset);
		hash_keys[i].key = *param_name;
		hash_keys[i].key_hash = ngx_hash_key_lc(param_name->data, param_name->len);
		hash_keys[i].value = &uri_param_defs[i];
	}

	hash.hash = &conf->uri_params_hash;
	hash.key = ngx_hash_key;
	hash.max_size = 512;
	hash.bucket_size = 64;
	hash.name = "uri_params_hash";
	hash.pool = cf->pool;
	hash.temp_pool = NULL;

	rc = ngx_hash_init(&hash, hash_keys, sizeof(hash_keys) / sizeof(hash_keys[0]));
	if (rc != NGX_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "ngx_hash_init failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_extract_uri_params(
	ngx_http_request_t* r,
	ngx_http_vod_loc_conf_t* conf,
	ngx_http_vod_suburi_params_t* suburi)
{
	ngx_http_vod_uri_param_def_t* param_def = NULL;
	ngx_uint_t  cur_key_hash = 0;
	ngx_str_t cur_param;
	ngx_int_t rc;
	uint32_t parsed_params_mask = 0;
	uint32_t param_index;
	u_char param_name[MAX_URI_PARAM_NAME_LEN + 1];
	u_char* param_name_end = param_name + sizeof(param_name);
	u_char* param_name_pos = param_name;
	u_char* copy_start = suburi->uri.data;
	u_char* cur_pos;
	u_char* end_pos = suburi->uri.data + suburi->uri.len;
	u_char* last_slash = NULL;
	u_char* p;

	// set defaults
	suburi->clip_from = 0;
	suburi->clip_to = UINT_MAX;
	suburi->required_tracks[MEDIA_TYPE_AUDIO] = 0xffffffff;
	suburi->required_tracks[MEDIA_TYPE_VIDEO] = 0xffffffff;
	suburi->speed_nom = 1;
	suburi->speed_denom = 1;

	p = ngx_palloc(r->pool, suburi->uri.len + 1);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_extract_uri_params: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	suburi->stripped_uri.data = p;

	for (cur_pos = suburi->uri.data; cur_pos <= end_pos; cur_pos++)
	{
		if (cur_pos < end_pos && *cur_pos != '/')
		{
			if (param_name_pos < param_name_end)
			{
				*param_name_pos = ngx_tolower(*cur_pos);
				cur_key_hash = ngx_hash(cur_key_hash, *param_name_pos);
				param_name_pos++;
			}
			continue;
		}

		if (last_slash == NULL)
		{
			last_slash = cur_pos;
			cur_key_hash = 0;
			param_name_pos = param_name;
			continue;
		}

		if (param_def == NULL)
		{
			param_def = ngx_hash_find(&conf->uri_params_hash, cur_key_hash, param_name, param_name_pos - param_name);
			if (param_def != NULL)
			{
				p = ngx_copy(p, copy_start, last_slash - copy_start);
				copy_start = last_slash;
			}
		}
		else
		{
			param_index = param_def - uri_param_defs;
			if ((parsed_params_mask & (1 << param_index)) == 0)		// first instance of a param takes priority
			{
				parsed_params_mask |= (1 << param_index);
				cur_param.data = last_slash + 1;
				cur_param.len = cur_pos - (last_slash + 1);
				rc = param_def->parser(&cur_param, suburi, param_def->target_offset);
				if (rc != NGX_OK)
				{
					ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
						"ngx_http_vod_extract_uri_params: %V parser failed %i", 
							(ngx_str_t*)((u_char*)conf + param_def->name_conf_offset), rc);
					return rc;
				}
			}
			copy_start = cur_pos;
			param_def = NULL;
		}

		last_slash = cur_pos;
		cur_key_hash = 0;
		param_name_pos = param_name;
	}

	if (suburi->clip_from >= suburi->clip_to)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_extract_uri_params: clip from %uD is larger than clip to %uD", suburi->clip_from, suburi->clip_to);
		return NGX_HTTP_BAD_REQUEST;
	}

	p = ngx_copy(p, copy_start, end_pos - copy_start);
	*p = '\0';

	suburi->stripped_uri.len = p - suburi->stripped_uri.data;

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_parse_uri_path(
	ngx_http_request_t* r,
	ngx_http_vod_loc_conf_t* conf,
	ngx_str_t* uri,
	ngx_http_vod_request_params_t* request_params)
{
	ngx_http_vod_suburi_params_t* suburis;
	ngx_http_vod_suburi_params_t cur_suburi;
	ngx_http_vod_multi_uri_t multi_uri;
	ngx_str_t parts[3];
	ngx_int_t rc;
	uint32_t i;
	int uri_count;

	rc = ngx_http_vod_parse_multi_uri(r, uri, &conf->multi_uri_suffix, &multi_uri);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: ngx_http_vod_parse_multi_uri failed", rc);
		return rc;
	}

	uri_count = 0;
	for (i = 0; i < multi_uri.parts_count; i++)
	{
		if ((request_params->required_files & (1 << i)) != 0)
		{
			uri_count++;
		}
	}

	if (uri_count == 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: request has no uris");
		return NGX_HTTP_BAD_REQUEST;
	}

	suburis = ngx_palloc(r->pool, sizeof(suburis[0]) * uri_count);
	if (suburis == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	parts[0] = multi_uri.prefix;
	parts[2] = multi_uri.postfix;

	uri_count = 0;
	for (i = 0; i < multi_uri.parts_count; i++)
	{
		if ((request_params->required_files & (1 << i)) == 0)
		{
			continue;
		}

		parts[1] = multi_uri.middle_parts[i];
		rc = ngx_http_vod_merge_string_parts(r, parts, 3, &cur_suburi.uri);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_path: ngx_http_vod_merge_string_parts failed", rc);
			return rc;
		}

		rc = ngx_http_vod_extract_uri_params(r, conf, &cur_suburi);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_path: ngx_http_vod_extract_uri_params failed", rc);
			return rc;
		}

		cur_suburi.required_tracks[MEDIA_TYPE_AUDIO] &= request_params->required_tracks[MEDIA_TYPE_AUDIO];
		cur_suburi.required_tracks[MEDIA_TYPE_VIDEO] &= request_params->required_tracks[MEDIA_TYPE_VIDEO];

		if (cur_suburi.required_tracks[MEDIA_TYPE_AUDIO] == 0 &&
			cur_suburi.required_tracks[MEDIA_TYPE_VIDEO] == 0)
		{
			continue;
		}

		cur_suburi.file_index = i;

		suburis[uri_count] = cur_suburi;
		uri_count++;
	}

	// need to test again since we filtered sub uris that didn't have any required tracks
	if (uri_count <= 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: request has no uris after track filtering");
		return NGX_HTTP_BAD_REQUEST;
	}

	request_params->suburis = suburis;
	request_params->suburi_count = uri_count;
	request_params->suburis_end = suburis + uri_count;
	request_params->uses_multi_uri = (multi_uri.parts_count > 1);

	return NGX_OK;
}
