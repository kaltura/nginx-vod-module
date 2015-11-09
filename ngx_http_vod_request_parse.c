#include "ngx_http_vod_request_parse.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_conf.h"
#include "ngx_http_vod_utils.h"
#include "vod/filters/rate_filter.h"
#include "vod/parse_utils.h"

// typedefs
typedef ngx_int_t(*ngx_http_vod_param_parser_t)(ngx_str_t* value, void* output, int offset);

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
ngx_http_vod_extract_sequence_tokens(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t sequence_index;

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

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &sequence_index);

		if (sequence_index == 0)
		{
			// no index => all files
			*result = 0xffffffff;
		}
		else
		{
			*result |= (1 << (sequence_index - 1));
		}

		if (start_pos < end_pos && *start_pos == '-')
		{
			start_pos++;
		}
	}
	return start_pos;
}

static u_char*
ngx_http_vod_extract_clip_token(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t clip_index;

	// by default use all files
	if (start_pos >= end_pos || *start_pos != 'c')
	{
		*result = INVALID_CLIP_INDEX;
		return start_pos;
	}

	start_pos++;		// skip the c

	start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &clip_index);
	if (clip_index == 0)
	{
		*result = INVALID_CLIP_INDEX;
	}
	else
	{
		*result = clip_index - 1;
	}

	if (start_pos < end_pos && *start_pos == '-')
	{
		start_pos++;
	}

	return start_pos;
}

ngx_int_t
ngx_http_vod_parse_uri_file_name(
	ngx_http_request_t* r,
	u_char* start_pos,
	u_char* end_pos,
	bool_t expect_segment_index,
	request_params_t* result)
{
	if (start_pos < end_pos && *start_pos == '-')
	{
		start_pos++;
	}

	if (expect_segment_index)
	{
		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &result->segment_index);
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

	start_pos = ngx_http_vod_extract_clip_token(start_pos, end_pos, &result->clip_index);

	start_pos = ngx_http_vod_extract_sequence_tokens(start_pos, end_pos, &result->sequences_mask);

	start_pos = parse_utils_extract_track_tokens(start_pos, end_pos, result->tracks_mask);

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
ngx_http_vod_parse_uint32_param(ngx_str_t* value, void* output, int offset)
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
ngx_http_vod_parse_tracks_param(ngx_str_t* value, void* output, int offset)
{
	uint32_t* tracks_mask = (uint32_t*)((u_char*)output + offset);
	u_char* end_pos;

	tracks_mask[MEDIA_TYPE_AUDIO] = 0;
	tracks_mask[MEDIA_TYPE_VIDEO] = 0;
	end_pos = parse_utils_extract_track_tokens(value->data, value->data + value->len, tracks_mask);
	if (end_pos != value->data + value->len)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	return NGX_OK;
}

static ngx_http_vod_uri_param_def_t uri_param_defs[] = {
	{ offsetof(ngx_http_vod_loc_conf_t, clip_to_param_name), ngx_http_vod_parse_uint32_param, offsetof(media_clip_source_t, clip_to) },
	{ offsetof(ngx_http_vod_loc_conf_t, clip_from_param_name), ngx_http_vod_parse_uint32_param, offsetof(media_clip_source_t, clip_from) },
	{ offsetof(ngx_http_vod_loc_conf_t, tracks_param_name), ngx_http_vod_parse_tracks_param, offsetof(media_clip_source_t, tracks_mask) },
	{ offsetof(ngx_http_vod_loc_conf_t, speed_param_name), NULL, 0 },
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
	hash.bucket_size = ngx_align(64, ngx_cacheline_size);
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
	ngx_str_t* uri,
	media_sequence_t* sequence,
	uint32_t* clip_id,
	media_clip_source_t* source_clip,
	media_clip_t** result)
{
	ngx_http_vod_uri_param_def_t* param_def = NULL;
	request_context_t request_context;
	media_clip_t* rate_filter = NULL;
	ngx_uint_t  cur_key_hash = 0;
	ngx_str_t cur_param;
	ngx_int_t rc;
	uint32_t parsed_params_mask = 0;
	uint32_t param_index;
	u_char param_name[MAX_URI_PARAM_NAME_LEN + 1];
	u_char* param_name_end = param_name + sizeof(param_name);
	u_char* param_name_pos = param_name;
	u_char* copy_start = uri->data;
	u_char* cur_pos;
	u_char* end_pos = uri->data + uri->len;
	u_char* last_slash = NULL;
	u_char* p;

	// set the source defaults
	vod_memzero(source_clip, sizeof(*source_clip));

	source_clip->base.type = MEDIA_CLIP_SOURCE;
	source_clip->base.id = (*clip_id)++;

	source_clip->clip_to = UINT_MAX;
	source_clip->tracks_mask[MEDIA_TYPE_AUDIO] = 0xffffffff;
	source_clip->tracks_mask[MEDIA_TYPE_VIDEO] = 0xffffffff;
	source_clip->uri = *uri;
	source_clip->sequence = sequence;
	
	*result = &source_clip->base;

	// allocate the stripped uri
	p = ngx_palloc(r->pool, uri->len + 1);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_extract_uri_params: ngx_palloc failed (1)");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	source_clip->stripped_uri.data = p;

	for (cur_pos = uri->data; cur_pos <= end_pos; cur_pos++)
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

				if (param_def->name_conf_offset == offsetof(ngx_http_vod_loc_conf_t, speed_param_name))
				{
					request_context.pool = r->pool;
					request_context.log = r->connection->log;

					rc = rate_filter_create_from_string(
						&request_context,
						&cur_param,
						&source_clip->base, 
						&rate_filter);
					if (rc != VOD_OK)
					{
						return ngx_http_vod_status_to_ngx_error(rc);
					}

					rate_filter->id = (*clip_id)++;
					*result = rate_filter;
				}
				else
				{
					rc = param_def->parser(&cur_param, source_clip, param_def->target_offset);
					if (rc != NGX_OK)
					{
						ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
							"ngx_http_vod_extract_uri_params: %V parser failed %i",
							(ngx_str_t*)((u_char*)conf + param_def->name_conf_offset), rc);
						return rc;
					}
				}
			}
			copy_start = cur_pos;
			param_def = NULL;
		}

		last_slash = cur_pos;
		cur_key_hash = 0;
		param_name_pos = param_name;
	}

	if (source_clip->clip_from >= source_clip->clip_to)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_extract_uri_params: clip from %uD is larger than clip to %uD", source_clip->clip_from, source_clip->clip_to);
		return NGX_HTTP_BAD_REQUEST;
	}

	p = ngx_copy(p, copy_start, end_pos - copy_start);
	*p = '\0';

	source_clip->stripped_uri.len = p - source_clip->stripped_uri.data;
	source_clip->mapped_uri = source_clip->stripped_uri;

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_parse_uri_path(
	ngx_http_request_t* r,
	ngx_http_vod_loc_conf_t* conf,
	ngx_str_t* uri,
	request_params_t* request_params,
	media_set_t* media_set)
{
	media_sequence_t* cur_sequence;
	media_clip_source_t** cur_source_ptr;
	media_clip_source_t* cur_source;
	ngx_http_vod_multi_uri_t multi_uri;
	media_clip_t** cur_clip_ptr;
	media_clip_t* cur_clip;
	ngx_str_t parts[3];
	ngx_str_t cur_uri;
	ngx_int_t rc;
	uint32_t sequences_mask;
	uint32_t parts_mask;
	uint32_t clip_id = 1;
	uint32_t i;
	int uri_count;

	rc = ngx_http_vod_parse_multi_uri(r, uri, &conf->multi_uri_suffix, &multi_uri);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: ngx_http_vod_parse_multi_uri failed", rc);
		return rc;
	}

	if (multi_uri.parts_count > 1)
	{
		sequences_mask = request_params->sequences_mask;
		request_params->sequences_mask = 0xffffffff;	// reset the sequences mask so that it won't be applied again on the mapping request
	}
	else
	{
		sequences_mask = 0xffffffff;
	}

	parts_mask = (1 << multi_uri.parts_count) - 1;
	
	uri_count = vod_get_number_of_set_bits(sequences_mask & parts_mask);
	if (uri_count == 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: request has no uris");
		return NGX_HTTP_BAD_REQUEST;
	}

	cur_sequence = ngx_palloc(r->pool,
		(sizeof(*cur_sequence) + sizeof(*cur_source_ptr) + sizeof(*cur_source) + sizeof(*cur_clip_ptr)) * uri_count);
	if (cur_sequence == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	media_set->sequences = cur_sequence;

	cur_source_ptr = (void*)(cur_sequence + uri_count);
	media_set->sources = cur_source_ptr;

	cur_source = (void*)(cur_source_ptr + uri_count);

	cur_clip_ptr = (void*)(cur_source + uri_count);

	parts[0] = multi_uri.prefix;
	parts[2] = multi_uri.postfix;

	for (i = 0; i < multi_uri.parts_count; i++)
	{
		if ((sequences_mask & (1 << i)) == 0)
		{
			continue;
		}

		parts[1] = multi_uri.middle_parts[i];
		rc = ngx_http_vod_merge_string_parts(r, parts, 3, &cur_uri);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_path: ngx_http_vod_merge_string_parts failed", rc);
			return rc;
		}

		rc = ngx_http_vod_extract_uri_params(r, conf, &cur_uri, cur_sequence, &clip_id, cur_source, &cur_clip);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_path: ngx_http_vod_extract_uri_params failed", rc);
			return rc;
		}

		if ((cur_source->tracks_mask[MEDIA_TYPE_AUDIO] & request_params->tracks_mask[MEDIA_TYPE_AUDIO]) == 0 &&
			(cur_source->tracks_mask[MEDIA_TYPE_VIDEO] & request_params->tracks_mask[MEDIA_TYPE_VIDEO]) == 0)
		{
			continue;
		}
		
		*cur_clip_ptr = cur_clip;
		*cur_source_ptr = cur_source;

		cur_sequence->clips = cur_clip_ptr;
		cur_sequence->index = i;
		cur_sequence->stripped_uri = cur_source->stripped_uri;
		cur_sequence->mapped_uri = cur_source->stripped_uri;

		cur_source++;
		cur_source_ptr++;
		cur_sequence++;
		cur_clip_ptr++;
	}

	// need to test again since we filtered sub uris that didn't have any required tracks
	media_set->sequence_count = cur_sequence - media_set->sequences;
	if (media_set->sequence_count <= 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: request has no uris after track filtering");
		return NGX_HTTP_BAD_REQUEST;
	}

	media_set->sequences_end = cur_sequence;
	media_set->has_multi_sequences = (multi_uri.parts_count > 1);
	media_set->sources_end = cur_source_ptr;
	media_set->total_clip_count = 1;
	media_set->clip_count = 1;
	media_set->durations = NULL;

	return NGX_OK;
}
