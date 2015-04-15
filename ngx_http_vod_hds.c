#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/hds/hds_manifest.h"
#include "vod/hds/hds_fragment.h"

// content types
static u_char f4m_content_type[] = "video/f4m";
static u_char f4f_content_type[] = "video/f4f";

// file extensions
static const u_char manifest_file_ext[] = ".f4m";

static ngx_int_t 
ngx_http_vod_hds_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	rc = hds_packager_build_manifest(
		&submodule_context->request_context,
		&submodule_context->conf->hds.manifest_config,
		&submodule_context->r->uri,
		&submodule_context->conf->segmenter,
		submodule_context->request_params.uses_multi_uri,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hds_handle_manifest: hds_packager_build_manifest failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = f4m_content_type;
	content_type->len = sizeof(f4m_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hds_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	read_cache_state_t* read_cache_state,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	hds_muxer_state_t* state;
	vod_status_t rc;	
	
	rc = hds_muxer_init_fragment(
		&submodule_context->request_context,
		submodule_context->request_params.segment_index,
		&submodule_context->mpeg_metadata,
		read_cache_state,
		segment_writer->write_tail,
		segment_writer->context,
		ngx_http_vod_submodule_size_only(submodule_context),
		output_buffer,
		response_size,
		&state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hds_init_frame_processor: hds_muxer_init_fragment failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)hds_muxer_process_frames;
	*frame_processor_state = state;

	// set the 'Content-type' header
	content_type->len = sizeof(f4f_content_type) - 1;
	content_type->data = (u_char *)f4f_content_type;

	return NGX_OK;
}

static const ngx_http_vod_request_t hds_manifest_request = {
	0,
	PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE,
	NULL,
	0,
	REQUEST_CLASS_MANIFEST,
	ngx_http_vod_hds_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t hds_fragment_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_hds_init_frame_processor,
};

static void
ngx_http_vod_hds_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_hds_loc_conf_t *conf)
{
}

static char *
ngx_http_vod_hds_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_hds_loc_conf_t *conf,
	ngx_http_vod_hds_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->manifest_config.fragment_file_name_prefix, prev->manifest_config.fragment_file_name_prefix, "frag");
	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");

	return NGX_CONF_OK;
}

static int
ngx_http_vod_hds_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_hds_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	ngx_http_vod_request_params_t* request_params)
{
	ngx_int_t rc;

	// fragment request
	if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hds.manifest_config.fragment_file_name_prefix))
	{
		// sample fragment file name: frag-f3-v1-a1-Seg1-Frag1
		start_pos += conf->hds.manifest_config.fragment_file_name_prefix.len;

		// parse the fragment index
		end_pos = ngx_http_vod_extract_uint32_token_reverse(start_pos, end_pos, &request_params->segment_index);
		if (request_params->segment_index == 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_hds_parse_uri_file_name: failed to parse fragment index");
			return NGX_HTTP_BAD_REQUEST;
		}

		request_params->segment_index--;		// convert to 0-based

		// extract the '-Seg1-Frag' part
		end_pos -= sizeof("-Seg1-Frag") - 1;
		if (end_pos < start_pos ||
			ngx_memcmp(end_pos, "-Seg1-Frag", sizeof("-Seg1-Frag") - 1) != 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_hds_parse_uri_file_name: invalid segment / fragment requested");
			return NGX_HTTP_BAD_REQUEST;
		}

		request_params->request = &hds_fragment_request;
	}
	// manifest request
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hds.manifest_file_name_prefix, manifest_file_ext))
	{
		start_pos += conf->hds.manifest_file_name_prefix.len;
		end_pos -= (sizeof(manifest_file_ext) - 1);
		request_params->request = &hds_manifest_request;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_hds_parse_uri_file_name: unidentified request");
		return NGX_HTTP_BAD_REQUEST;
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, FALSE, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_hds_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_hds_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
		"ngx_http_vod_hds_parse_drm_info: drm support for hds not implemented");
	return VOD_UNEXPECTED;
}

DEFINE_SUBMODULE(hds);
