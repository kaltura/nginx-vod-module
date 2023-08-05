#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/thumb/thumb_grabber.h"
#include "vod/manifest_utils.h"
#include "vod/parse_utils.h"

#define THUMB_TIMESCALE (1000)

// macros
#define skip_dash(start_pos, end_pos)	\
	if (start_pos >= end_pos)			\
	{									\
		return start_pos;				\
	}									\
	if (*start_pos == '-')				\
	{									\
		start_pos++;					\
		if (start_pos >= end_pos)		\
		{								\
			return start_pos;			\
		}								\
	}

static const u_char jpg_file_ext[] = ".jpg";
static u_char jpeg_content_type[] = "image/jpeg";

ngx_int_t 
ngx_http_vod_thumb_get_url(
	ngx_http_vod_submodule_context_t* submodule_context,
	uint32_t sequences_mask,
	ngx_str_t* result)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	ngx_http_request_t* r = submodule_context->r;
	request_params_t* request_params = &submodule_context->request_params;
	ngx_str_t request_params_str;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	size_t result_size;
	u_char* p;

	// get the base url
	rc = ngx_http_vod_get_base_url(
		r,
		conf->segments_base_url != NULL ? conf->segments_base_url : conf->base_url,
		&r->uri,
		&base_url);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_thumb_get_url: ngx_http_vod_get_base_url failed %i", rc);
		return rc;
	}

	// get the request params string
	rc = manifest_utils_build_request_params_string(
		&submodule_context->request_context,
		request_params->tracks_mask,
		INVALID_SEGMENT_INDEX,
		sequences_mask,
		NULL,
		NULL,
		request_params->tracks_mask,
		&request_params_str);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_thumb_get_url: manifest_utils_build_request_params_string failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(r, rc);
	}

	// get the result size
	result_size = base_url.len + conf->thumb.file_name_prefix.len + 
		1 + VOD_INT64_LEN + request_params_str.len + sizeof(jpg_file_ext) - 1;

	// allocate the result buffer
	p = ngx_pnalloc(submodule_context->request_context.pool, result_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, submodule_context->request_context.log, 0,
			"ngx_http_vod_thumb_get_url: ngx_pnalloc failed");
		return ngx_http_vod_status_to_ngx_error(r, VOD_ALLOC_FAILED);
	}

	result->data = p;

	// write the result
	if (base_url.len != 0)
	{
		p = vod_copy(p, base_url.data, base_url.len);
	}

	p = vod_copy(p, conf->thumb.file_name_prefix.data, conf->thumb.file_name_prefix.len);
	p = vod_sprintf(p, "-%uL", request_params->segment_time);
	p = vod_copy(p, request_params_str.data, request_params_str.len);
	p = vod_copy(p, jpg_file_ext, sizeof(jpg_file_ext) - 1);

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_thumb_get_url: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return ngx_http_vod_status_to_ngx_error(r, VOD_UNEXPECTED);
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_thumb_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	rc = thumb_grabber_init_state(
		&submodule_context->request_context,
		submodule_context->media_set.filtered_tracks,
		&submodule_context->request_params,
		submodule_context->conf->thumb.accurate,
		segment_writer->write_tail,
		segment_writer->context,
		frame_processor_state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_thumb_init_frame_processor: thumb_grabber_init_state failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)thumb_grabber_process;

	content_type->len = sizeof(jpeg_content_type) - 1;
	content_type->data = (u_char *)jpeg_content_type;

	return NGX_OK;
}

static const ngx_http_vod_request_t thumb_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_THUMB,
	VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(HEVC) | VOD_CODEC_FLAG(VP8) | VOD_CODEC_FLAG(VP9) | VOD_CODEC_FLAG(AV1),
	THUMB_TIMESCALE,
	NULL,
	ngx_http_vod_thumb_init_frame_processor,
};

static void
ngx_http_vod_thumb_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_thumb_loc_conf_t *conf)
{
	conf->accurate = NGX_CONF_UNSET;
}

static char *
ngx_http_vod_thumb_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_thumb_loc_conf_t *conf,
	ngx_http_vod_thumb_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->file_name_prefix, prev->file_name_prefix, "thumb");
	ngx_conf_merge_value(conf->accurate, prev->accurate, 1);
	return NGX_CONF_OK;
}

static int 
ngx_http_vod_thumb_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

#if (NGX_HAVE_LIB_SW_SCALE)
static u_char*
ngx_http_vod_thumb_parse_dimensions(
	ngx_http_request_t* r,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* result)
{
	skip_dash(start_pos, end_pos);

	// width
	if (*start_pos == 'w')
	{
		start_pos++;		// skip the w

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &result->width);
		if (result->width <= 0)
		{
			return NULL;
		}

		skip_dash(start_pos, end_pos);
	}

	// height
	if (*start_pos == 'h')
	{
		start_pos++;		// skip the h

		start_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &result->height);
		if (result->height <= 0)
		{
			return NULL;
		}

		skip_dash(start_pos, end_pos);
	}

	return start_pos;
}
#endif // NGX_HAVE_LIB_SW_SCALE

static ngx_int_t
ngx_http_vod_thumb_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	segment_time_type_t time_type;
	int64_t time;
	ngx_int_t rc;

	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->thumb.file_name_prefix, jpg_file_ext))
	{
		start_pos += conf->thumb.file_name_prefix.len;
		end_pos -= (sizeof(jpg_file_ext) - 1);
		*request = &thumb_request;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_thumb_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	// parse the time
	if (start_pos < end_pos && *start_pos == '-')
	{
		start_pos++;		// skip the -
	}

	time_type = SEGMENT_TIME_ABSOLUTE;
	if (start_pos < end_pos)
	{
		switch (*start_pos)
		{
		case '-':
			start_pos++;		// skip the -
			time_type = SEGMENT_TIME_END_RELATIVE;
			break;

		case '+':
			start_pos++;		// skip the +
			time_type = SEGMENT_TIME_START_RELATIVE;
			break;
		}
	}

	if (start_pos >= end_pos || *start_pos < '0' || *start_pos > '9')
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_thumb_parse_uri_file_name: failed to parse thumbnail time");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	time = 0;
	do 
	{
		time = time * 10 + *start_pos++ - '0';
	} while (start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9');

	if (time == INVALID_SEGMENT_TIME)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_thumb_parse_uri_file_name: failed to parse thumbnail time");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

#if (NGX_HAVE_LIB_SW_SCALE)
	start_pos = ngx_http_vod_thumb_parse_dimensions(r, start_pos, end_pos, request_params);
	if (start_pos == NULL)
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_thumb_parse_uri_file_name: failed to parse width/height");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}
#endif // NGX_HAVE_LIB_SW_SCALE

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, 0, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_thumb_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}
	
	request_params->segment_time = time;
	request_params->segment_time_type = time_type;
	vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_AUDIO]);
	vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_SUBTITLE]);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_thumb_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
		"ngx_http_vod_thumb_parse_drm_info: unexpected - drm enabled on thumbnail request");
	return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
}

DEFINE_SUBMODULE(thumb);
