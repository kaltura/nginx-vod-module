#include <ngx_http.h>

#if (NGX_HAVE_LIB_AV_CODEC)

#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/thumb/thumb_grabber.h"

#define THUMB_TIMESCALE (1000)

static const u_char jpg_file_ext[] = ".jpg";
static u_char jpeg_content_type[] = "image/jpeg";

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
		submodule_context->request_params.segment_time,
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
	VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(HEVC) | VOD_CODEC_FLAG(VP8) | VOD_CODEC_FLAG(VP9),
	THUMB_TIMESCALE,
	NULL,
	ngx_http_vod_thumb_init_frame_processor,
};

void
ngx_http_vod_thumb_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_thumb_loc_conf_t *conf)
{
}

static char *
ngx_http_vod_thumb_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_thumb_loc_conf_t *conf,
	ngx_http_vod_thumb_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->file_name_prefix, prev->file_name_prefix, "thumb");
	return NGX_CONF_OK;
}

static int 
ngx_http_vod_thumb_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_thumb_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	uint64_t time;
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

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, 0, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_thumb_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}
	
	request_params->segment_time = time;
	request_params->tracks_mask[MEDIA_TYPE_AUDIO] = 0;
	request_params->tracks_mask[MEDIA_TYPE_SUBTITLE] = 0;

	return NGX_OK;
}

ngx_int_t
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

#endif // (NGX_HAVE_LIB_AV_CODEC)
