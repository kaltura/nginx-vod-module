#include "ngx_http_vod_request_parse.h"
#include "ngx_http_vod_module.h"
#include "ngx_http_vod_conf.h"
#include "ngx_http_vod_utils.h"
#include "vod/filters/rate_filter.h"
#include "vod/parse_utils.h"

// macros
#define skip_dash(start_pos, end_pos)	\
	if (start_pos >= end_pos)			\
	{									\
		return NGX_OK;					\
	}									\
	if (*start_pos == '-')				\
	{									\
		start_pos++;					\
		if (start_pos >= end_pos)		\
		{								\
			return NGX_OK;				\
		}								\
	}

// typedefs
typedef ngx_int_t(*ngx_http_vod_param_parser_t)(ngx_str_t* value, void* output, int offset);

typedef struct {
	int name_conf_offset;
	char* name;
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
ngx_http_vod_extract_track_tokens(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t stream_index;
	int media_type;

	ngx_memzero(result, sizeof(result[0]) * MEDIA_TYPE_COUNT);

	for (;;)
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

		if (start_pos >= end_pos)
		{
			// no index => all streams of the media type
			result[media_type] = 0xffffffff;
			break;
		}

		if (*start_pos >= '0' && *start_pos <= '9')
		{
			stream_index = *start_pos - '0';
			start_pos++;

			if (start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9')
			{
				stream_index = stream_index * 10 + *start_pos - '0';
				start_pos++;
			}
		}
		else
		{
			stream_index = 0;
		}

		if (stream_index == 0)
		{
			// no index => all streams of the media type
			result[media_type] = 0xffffffff;
		}
		else
		{
			result[media_type] |= (1 << (stream_index - 1));
		}

		if (start_pos >= end_pos)
		{
			break;
		}

		if (*start_pos == '-')
		{
			start_pos++;
			if (start_pos >= end_pos)
			{
				break;			
			}
		}
	}

	return NULL;
}

ngx_int_t
ngx_http_vod_parse_uri_file_name(
	ngx_http_request_t* r,
	u_char* start_pos,
	u_char* end_pos,
	uint32_t flags,
	request_params_t* result)
{
	ngx_str_t* cur_sequence_id;
	ngx_str_t* last_sequence_id;
	uint32_t default_tracks_mask;
	uint32_t* tracks_mask;
	uint32_t* end_mask;
	uint32_t* cur_mask;
	uint32_t segment_index_shift;
	uint32_t masks_per_sequence;
	uint32_t sequence_index;
	uint32_t clip_index;
	uint32_t media_type;
	uint32_t pts_delay;
	uint32_t version;
	language_id_t lang_id;

	default_tracks_mask = (flags & PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE) ? 0xffffffff : 1;
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		result->tracks_mask[media_type] = default_tracks_mask;
	}
	result->sequences_mask = 0xffffffff;
	result->clip_index = INVALID_CLIP_INDEX;

	// segment index
	if ((flags & PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX) != 0)
	{
		if (start_pos < end_pos && *start_pos == '-')
		{
			start_pos++;		// skip the -
		}

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &result->segment_index);
		if (result->segment_index <= 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri_file_name: failed to parse segment index");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}
		result->segment_index--;		// convert to 0-based

		skip_dash(start_pos, end_pos);

		// index shift
		if (*start_pos == 'i')
		{
			start_pos++;		// skip the i

			start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &segment_index_shift);
			if (segment_index_shift <= 0)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_parse_uri_file_name: failed to parse segment index shift");
				return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
			}

			result->segment_index += segment_index_shift;

			skip_dash(start_pos, end_pos);
		}
	}
	else
	{
		skip_dash(start_pos, end_pos);
	}

	// clip index
	if (*start_pos == 'c' && (flags & PARSE_FILE_NAME_ALLOW_CLIP_INDEX) != 0)
	{
		start_pos++;		// skip the c

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &clip_index);
		if (clip_index <= 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri_file_name: failed to parse clip index");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		result->clip_index = clip_index - 1;

		skip_dash(start_pos, end_pos);
	}

	// sequence id
	if (*start_pos == 's')
	{
		cur_sequence_id = result->sequence_ids;
		last_sequence_id = cur_sequence_id + MAX_SEQUENCE_IDS;
		for (; cur_sequence_id < last_sequence_id; cur_sequence_id++)
		{
			start_pos++;		// skip the s

			cur_sequence_id->data = start_pos;

			while (start_pos < end_pos && *start_pos != '-')
			{
				start_pos++;
			}

			cur_sequence_id->len = start_pos - cur_sequence_id->data;

			skip_dash(start_pos, end_pos);

			if (*start_pos != 's')
			{
				break;
			}
		}
	}

	// sequence (file) index
	if (*start_pos == 'f')
	{
		tracks_mask = result->tracks_mask;
		masks_per_sequence = 0;
		result->sequences_mask = 0;

		for (;;)
		{
			start_pos++;		// skip the f

			if (start_pos >= end_pos || *start_pos < '1' || *start_pos > '9')
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_parse_uri_file_name: missing index following sequence selector");
				return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
			}

			sequence_index = *start_pos - '0';
			start_pos++;		// skip the digit

			if (start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9')
			{
				sequence_index = sequence_index * 10 + *start_pos - '0';
				if (sequence_index > MAX_SEQUENCES)
				{
					ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
						"ngx_http_vod_parse_uri_file_name: sequence index too big");
					return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
				}
				start_pos++;		// skip the digit
			}

			sequence_index--;		// Note: sequence_index cannot be 0 here
			result->sequences_mask |= (1 << sequence_index);

			skip_dash(start_pos, end_pos);

			if (*start_pos == 'v' || *start_pos == 'a')
			{
				start_pos = ngx_http_vod_extract_track_tokens(
					start_pos, 
					end_pos, 
					tracks_mask + masks_per_sequence * sequence_index);
				if (start_pos == NULL)
				{
					return NGX_OK;
				}
			}

			if (*start_pos != 'f')
			{
				break;
			}

			if (result->sequence_tracks_mask != NULL)
			{
				continue;
			}

			// more than one sequence, allocate the per sequence tracks mask
			result->sequence_tracks_mask = ngx_palloc(r->pool, 
				sizeof(result->sequence_tracks_mask[0]) * MEDIA_TYPE_COUNT * MAX_SEQUENCES);
			if (result->sequence_tracks_mask == NULL)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
					"ngx_http_vod_parse_uri_file_name: ngx_palloc failed");
				return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
			}

			// initialize the mask with the default
			cur_mask = result->sequence_tracks_mask;
			end_mask = cur_mask + MEDIA_TYPE_COUNT * MAX_SEQUENCES;
			for (; cur_mask < end_mask; cur_mask++)
			{
				*cur_mask = default_tracks_mask;
			}

			// copy the currently parsed mask to its place
			tracks_mask = result->sequence_tracks_mask + sequence_index * MEDIA_TYPE_COUNT;
			ngx_memcpy(tracks_mask, result->tracks_mask, sizeof(tracks_mask[0]) * MEDIA_TYPE_COUNT);

			// restore the global mask to the default
			for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
			{
				result->tracks_mask[media_type] = default_tracks_mask;
			}

			// from now on, parse directly to the sequence tracks mask
			tracks_mask = result->sequence_tracks_mask;
			masks_per_sequence = MEDIA_TYPE_COUNT;
		}
	}
	else if (*start_pos == 'v' || *start_pos == 'a')
	{
		// tracks
		start_pos = ngx_http_vod_extract_track_tokens(start_pos, end_pos, result->tracks_mask);
		if (start_pos == NULL)
		{
			return NGX_OK;
		}
	}

	// pts delay
	if (*start_pos == 'p')
	{
		start_pos++;		// skip the p

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &pts_delay);
		if (pts_delay <= 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri_file_name: failed to parse pts delay");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		result->pts_delay = pts_delay;

		skip_dash(start_pos, end_pos);
	}

	// languages
	if (*start_pos == 'l')
	{
		result->langs_mask = ngx_pnalloc(r->pool, LANG_MASK_SIZE);
		if (result->langs_mask == NULL)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_file_name: ngx_pnalloc failed");
			return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
		}

		ngx_memzero(result->langs_mask, LANG_MASK_SIZE);

		for (;;)
		{
			start_pos++;		// skip the l
			if (start_pos + LANG_ISO639_2_LEN > end_pos)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_parse_uri_file_name: language specifier length must be 3 characters");
				return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
			}

			lang_id = lang_parse_iso639_2_code(iso639_2_str_to_int(start_pos));
			if (lang_id == 0)
			{
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_http_vod_parse_uri_file_name: failed to parse language specifier %*s", (size_t)3, start_pos);
				return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
			}

			vod_set_bit(result->langs_mask, lang_id);

			start_pos += LANG_ISO639_2_LEN;

			skip_dash(start_pos, end_pos);

			if (*start_pos != 'l')
			{
				break;
			}
		}
	}

	// version
	if (*start_pos == 'x')
	{
		start_pos++;		// skip the x

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &version);
		if (version <= 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_parse_uri_file_name: failed to parse version");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		result->version = version - 1;

		skip_dash(start_pos, end_pos);
	}

	ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
		"ngx_http_vod_parse_uri_file_name: did not consume the whole name");
	return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
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
	result->postfix.data = NULL;
	result->postfix.len = 0;

	if (uri->len < multi_uri_suffix->len ||
		ngx_memcmp(multi_uri_suffix->data, uri->data + uri->len - multi_uri_suffix->len, multi_uri_suffix->len) != 0)
	{
		// not a multi uri
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
				return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
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
ngx_http_vod_parse_uint64_param(ngx_str_t* value, void* output, int offset)
{
	ngx_int_t result;

	result = ngx_atoi(value->data, value->len);
	if (result < 0)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	*(uint64_t*)((u_char*)output + offset) = result;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_parse_tracks_param(ngx_str_t* value, void* output, int offset)
{
	uint32_t* tracks_mask = (uint32_t*)((u_char*)output + offset);
	u_char* end_pos;

	ngx_memzero(tracks_mask, sizeof(tracks_mask[0]) * MEDIA_TYPE_COUNT);
	end_pos = parse_utils_extract_track_tokens(value->data, value->data + value->len, tracks_mask);
	if (end_pos != value->data + value->len)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_parse_lang_param(ngx_str_t* value, void* output, int offset)
{
	media_clip_source_t* clip = output;
	media_sequence_t* sequence = clip->sequence;
	language_id_t result;

	if (value->len < LANG_ISO639_2_LEN)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	result = lang_parse_iso639_2_code(iso639_2_str_to_int(value->data));
	if (result == 0)
	{
		return NGX_HTTP_BAD_REQUEST;
	}

	sequence->language = result;
	lang_get_native_name(result, &sequence->label);

	return VOD_OK;
}

static ngx_http_vod_uri_param_def_t uri_param_defs[] = {
	{ offsetof(ngx_http_vod_loc_conf_t, clip_to_param_name), "clip to", ngx_http_vod_parse_uint64_param, offsetof(media_clip_source_t, clip_to) },
	{ offsetof(ngx_http_vod_loc_conf_t, clip_from_param_name), "clip from", ngx_http_vod_parse_uint64_param, offsetof(media_clip_source_t, clip_from) },
	{ offsetof(ngx_http_vod_loc_conf_t, tracks_param_name), "tracks", ngx_http_vod_parse_tracks_param, offsetof(media_clip_source_t, tracks_mask) },
	{ offsetof(ngx_http_vod_loc_conf_t, lang_param_name), "lang", ngx_http_vod_parse_lang_param, 0 },
	{ offsetof(ngx_http_vod_loc_conf_t, speed_param_name), "speed", NULL, 0 },
	{ -1, NULL, NULL, 0}
};

static ngx_http_vod_uri_param_def_t pd_uri_param_defs[] = {
	{ offsetof(ngx_http_vod_loc_conf_t, clip_to_param_name), "clip to", ngx_http_vod_parse_uint64_param, offsetof(media_clip_source_t, clip_to) },
	{ offsetof(ngx_http_vod_loc_conf_t, clip_from_param_name), "clip from", ngx_http_vod_parse_uint64_param, offsetof(media_clip_source_t, clip_from) },
	{ -1, NULL, NULL, 0 }
};

static ngx_int_t
ngx_http_vod_init_hash(
	ngx_conf_t *cf, 
	ngx_http_vod_uri_param_def_t* elements,
	ngx_http_vod_loc_conf_t* conf, 
	char* hash_name, 
	ngx_hash_t* output)
{
	ngx_http_vod_uri_param_def_t *element;
	ngx_array_t elements_arr;
	ngx_hash_key_t *hash_key;
	ngx_hash_init_t hash;
	ngx_str_t* cur_key;

	if (ngx_array_init(&elements_arr, cf->temp_pool, 32, sizeof(ngx_hash_key_t)) != NGX_OK)
	{
		return NGX_ERROR;
	}

	for (element = elements; element->name_conf_offset >= 0; element++)
	{
		cur_key = (ngx_str_t*)((u_char*)conf + element->name_conf_offset);
		if (cur_key->len == 0)
		{
			break;
		}

		hash_key = ngx_array_push(&elements_arr);
		if (hash_key == NULL)
		{
			return NGX_ERROR;
		}

		hash_key->key = *cur_key;
		hash_key->key_hash = ngx_hash_key_lc(cur_key->data, cur_key->len);
		hash_key->value = element;
	}

	hash.hash = output;
	hash.key = ngx_hash_key_lc;
	hash.max_size = 512;
	hash.bucket_size = ngx_align(64, ngx_cacheline_size);
	hash.name = hash_name;
	hash.pool = cf->pool;
	hash.temp_pool = NULL;

	if (ngx_hash_init(&hash, elements_arr.elts, elements_arr.nelts) != NGX_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_init_uri_params_hash(ngx_conf_t *cf, ngx_http_vod_loc_conf_t* conf)
{
	ngx_int_t rc;

	rc = ngx_http_vod_init_hash(
		cf,
		uri_param_defs,
		conf,
		"uri_params_hash",
		&conf->uri_params_hash);
	if (rc != NGX_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "failed to initialize uri params hash");
		return rc;
	}

	rc = ngx_http_vod_init_hash(
		cf,
		pd_uri_param_defs,
		conf,
		"pd_uri_params_hash",
		&conf->pd_uri_params_hash);
	if (rc != NGX_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "failed to initialize progressive download uri params hash");
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_extract_uri_params(
	ngx_http_request_t* r,
	ngx_hash_t* params_hash,
	ngx_str_t* uri,
	media_sequence_t* sequence,
	uint32_t* clip_id,
	media_clip_source_t* source_clip,
	media_clip_t** result)
{
	ngx_http_vod_uri_param_def_t* param_def = NULL;
	media_clip_rate_filter_t* rate_filter = NULL;
	request_context_t request_context;
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
	ngx_memzero(source_clip, sizeof(*source_clip));

	source_clip->base.type = MEDIA_CLIP_SOURCE;
	source_clip->base.id = (*clip_id)++;

	source_clip->clip_to = ULLONG_MAX;
	ngx_memset(source_clip->tracks_mask, 0xff, sizeof(source_clip->tracks_mask));
	source_clip->uri = *uri;
	source_clip->sequence = sequence;
	
	*result = &source_clip->base;

	// allocate the stripped uri
	p = ngx_palloc(r->pool, uri->len + 1);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_extract_uri_params: ngx_palloc failed (1)");
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
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
			param_def = ngx_hash_find(params_hash, cur_key_hash, param_name, param_name_pos - param_name);
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
						return ngx_http_vod_status_to_ngx_error(r, rc);
					}

					if (rate_filter->rate.num != rate_filter->rate.denom)
					{
						rate_filter->base.id = (*clip_id)++;
						*result = &rate_filter->base;
					}
				}
				else
				{
					rc = param_def->parser(&cur_param, source_clip, param_def->target_offset);
					if (rc != NGX_OK)
					{
						ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
							"ngx_http_vod_extract_uri_params: %s parser failed %i", param_def->name, rc);
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
			"ngx_http_vod_extract_uri_params: clip from %uL is larger than clip to %uL", source_clip->clip_from, source_clip->clip_to);
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
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
	ngx_str_t* multi_uri_suffix,
	ngx_hash_t* params_hash,
	ngx_str_t* uri,
	request_params_t* request_params,
	media_set_t* media_set)
{
	media_sequence_t* cur_sequence;
	media_clip_source_t* cur_source;
	media_clip_source_t* sources_head;
	ngx_http_vod_multi_uri_t multi_uri;
	media_clip_t** cur_clip_ptr;
	media_clip_t* cur_clip;
	ngx_str_t parts[3];
	ngx_str_t cur_uri;
	ngx_int_t rc;
	uint32_t sequences_mask;
	uint32_t parts_mask;
	uint32_t media_type;
	uint32_t clip_id = 1;
	uint32_t i;
	bool_t has_tracks;
	int uri_count;

	media_set->uri = *uri;		// must save the uri before calling ngx_http_vod_parse_multi_uri as it may change

	multi_uri.parts_count = 0;

	rc = ngx_http_vod_parse_multi_uri(r, uri, multi_uri_suffix, &multi_uri);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: ngx_http_vod_parse_multi_uri failed %i", rc);
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
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	cur_sequence = ngx_palloc(r->pool,
		(sizeof(*cur_sequence) + sizeof(*cur_source) + sizeof(*cur_clip_ptr)) * uri_count);
	if (cur_sequence == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: ngx_palloc failed");
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
	}
	media_set->sequences = cur_sequence;

	cur_source = (void*)(cur_sequence + uri_count);

	cur_clip_ptr = (void*)(cur_source + uri_count);

	sources_head = NULL;

	parts[0] = multi_uri.prefix;
	parts[2] = multi_uri.postfix;

	for (i = 0; i < multi_uri.parts_count; i++)
	{
		if ((sequences_mask & (1 << i)) == 0)
		{
			continue;
		}

		cur_sequence->id.len = 0;
		cur_sequence->language = 0;
		cur_sequence->label.len = 0;
		cur_sequence->first_key_frame_offset = 0;
		cur_sequence->key_frame_durations = NULL;
		cur_sequence->drm_info = NULL;
		vod_memzero(cur_sequence->bitrate, sizeof(cur_sequence->bitrate));

		parts[1] = multi_uri.middle_parts[i];
		rc = ngx_http_vod_merge_string_parts(r, parts, 3, &cur_uri);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_path: ngx_http_vod_merge_string_parts failed %i", rc);
			return rc;
		}

		rc = ngx_http_vod_extract_uri_params(r, params_hash, &cur_uri, cur_sequence, &clip_id, cur_source, &cur_clip);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_parse_uri_path: ngx_http_vod_extract_uri_params failed %i", rc);
			return rc;
		}

		has_tracks = FALSE;
		for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
		{
			if ((cur_source->tracks_mask[media_type] & request_params->tracks_mask[media_type]) != 0)
			{
				has_tracks = TRUE;
				break;
			}
		}

		if (!has_tracks)
		{
			continue;
		}
		
		*cur_clip_ptr = cur_clip;

		cur_source->next = sources_head;
		sources_head = cur_source;

		cur_sequence->clips = cur_clip_ptr;
		cur_sequence->index = i;
		cur_sequence->stripped_uri = cur_source->stripped_uri;
		cur_sequence->mapped_uri = cur_source->stripped_uri;

		cur_source++;
		cur_sequence++;
		cur_clip_ptr++;
	}

	// need to test again since we filtered sub uris that didn't have any required tracks
	media_set->sequence_count = cur_sequence - media_set->sequences;
	if (media_set->sequence_count <= 0)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_parse_uri_path: request has no uris after track filtering");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	media_set->sources_head = sources_head;
	media_set->sequences_end = cur_sequence;
	media_set->has_multi_sequences = (multi_uri.parts_count > 1);
	media_set->timing.total_count = 1;
	media_set->clip_count = 1;
	media_set->presentation_end = TRUE;

	return NGX_OK;
}
