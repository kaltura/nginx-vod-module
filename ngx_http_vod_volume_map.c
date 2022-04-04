#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/filters/volume_map.h"

#define VOLUME_MAP_TIMESCALE (1000)

static const u_char csv_file_ext[] = ".csv";
static u_char csv_content_type[] = "text/csv";
static ngx_str_t csv_header = ngx_string("pts,rms_level\n");

static ngx_int_t
ngx_http_vod_volume_map_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	rc = volume_map_writer_init(
		&submodule_context->request_context,
		&submodule_context->media_set,
		submodule_context->conf->volume_map.interval,
		segment_writer->write_tail,
		segment_writer->context,
		frame_processor_state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_volume_map_init_frame_processor: volume_map_writer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)volume_map_writer_process;

	*output_buffer = csv_header;
	content_type->len = sizeof(csv_content_type) - 1;
	content_type->data = (u_char *)csv_content_type;

	return NGX_OK;
}

static const ngx_http_vod_request_t volume_map_request = {
	REQUEST_FLAG_SINGLE_TRACK | REQUEST_FLAG_PARSE_ALL_CLIPS,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_OTHER,
	VOD_CODEC_FLAG(AAC),
	VOLUME_MAP_TIMESCALE,
	NULL,
	ngx_http_vod_volume_map_init_frame_processor,
};

static void
ngx_http_vod_volume_map_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_volume_map_loc_conf_t *conf)
{
	conf->interval = NGX_CONF_UNSET_UINT;
}

static char *
ngx_http_vod_volume_map_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_volume_map_loc_conf_t *conf,
	ngx_http_vod_volume_map_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->file_name_prefix, prev->file_name_prefix, "volume_map");
	ngx_conf_merge_uint_value(conf->interval, prev->interval, 1000);
	return NGX_CONF_OK;
}

static int 
ngx_http_vod_volume_map_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_volume_map_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	ngx_int_t rc;

	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->volume_map.file_name_prefix, csv_file_ext))
	{
		start_pos += conf->volume_map.file_name_prefix.len;
		end_pos -= (sizeof(csv_file_ext) - 1);
		*request = &volume_map_request;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_volume_map_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, 0, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_volume_map_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_VIDEO]);
	vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_SUBTITLE]);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_volume_map_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
		"ngx_http_vod_volume_map_parse_drm_info: unexpected - drm enabled on volume map request");
	return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
}

DEFINE_SUBMODULE(volume_map);
