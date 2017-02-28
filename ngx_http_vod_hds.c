#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/hds/hds_manifest.h"
#include "vod/hds/hds_fragment.h"
#include "vod/udrm.h"

// constants
#define SUPPORTED_CODECS (VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(AAC) | VOD_CODEC_FLAG(MP3))

// content types
static u_char f4m_content_type[] = "video/f4m";
static u_char f4f_content_type[] = "video/f4f";
static u_char abst_content_type[] = "video/abst";

// file extensions
static const u_char manifest_file_ext[] = ".f4m";
static const u_char bootstrap_file_ext[] = ".abst";

static ngx_int_t 
ngx_http_vod_hds_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	if (conf->hds.absolute_manifest_urls)
	{
		rc = ngx_http_vod_get_base_url(submodule_context->r, conf->base_url, &submodule_context->r->uri, &base_url);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	rc = hds_packager_build_manifest(
		&submodule_context->request_context,
		&conf->hds.manifest_config,
		&base_url,
		&submodule_context->r->uri,
		&submodule_context->media_set,
		conf->drm_enabled,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hds_handle_manifest: hds_packager_build_manifest failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	content_type->data = f4m_content_type;
	content_type->len = sizeof(f4m_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hds_handle_bootstrap(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;
	
	rc = hds_packager_build_bootstrap(
		&submodule_context->request_context,
		&submodule_context->media_set,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hds_handle_bootstrap: hds_packager_build_bootstrap failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	content_type->data = abst_content_type;
	content_type->len = sizeof(abst_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hds_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	hds_muxer_state_t* state;
	hds_encryption_params_t encryption_params;
	vod_status_t rc;
	drm_info_t* drm_info;

	if (submodule_context->conf->drm_enabled)
	{
		drm_info = submodule_context->media_set.sequences[0].drm_info;

		encryption_params.type = HDS_ENC_SELECTIVE;
		encryption_params.key = drm_info->key;
		encryption_params.iv = submodule_context->media_set.sequences[0].encryption_key;
	}
	else
	{
		encryption_params.type = HDS_ENC_NONE;
	}

	rc = hds_muxer_init_fragment(
		&submodule_context->request_context,
		&submodule_context->conf->hds.fragment_config,
		&encryption_params,
		submodule_context->request_params.segment_index,
		&submodule_context->media_set,
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
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
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
	REQUEST_CLASS_MANIFEST,
	SUPPORTED_CODECS,
	HDS_TIMESCALE,
	ngx_http_vod_hds_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t hds_bootstrap_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE | REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE,
	0,
	REQUEST_CLASS_MANIFEST,
	SUPPORTED_CODECS,
	HDS_TIMESCALE,
	ngx_http_vod_hds_handle_bootstrap,
	NULL,
};

static const ngx_http_vod_request_t hds_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS,
	HDS_TIMESCALE,
	NULL,
	ngx_http_vod_hds_init_frame_processor,
};

static void
ngx_http_vod_hds_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_hds_loc_conf_t *conf)
{
	conf->absolute_manifest_urls = NGX_CONF_UNSET;
	conf->fragment_config.generate_moof_atom = NGX_CONF_UNSET;
}

static char *
ngx_http_vod_hds_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_hds_loc_conf_t *conf,
	ngx_http_vod_hds_loc_conf_t *prev)
{
	ngx_conf_merge_value(conf->absolute_manifest_urls, prev->absolute_manifest_urls, 0);
	ngx_conf_merge_str_value(conf->manifest_config.fragment_file_name_prefix, prev->manifest_config.fragment_file_name_prefix, "frag");
	ngx_conf_merge_str_value(conf->manifest_config.bootstrap_file_name_prefix, prev->manifest_config.bootstrap_file_name_prefix, "bootstrap");
	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");
	ngx_conf_merge_value(conf->fragment_config.generate_moof_atom, prev->fragment_config.generate_moof_atom, 1);

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
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	uint32_t flags = 0;
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
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		request_params->segment_index--;		// convert to 0-based

		// extract the '-Seg1-Frag' part
		end_pos -= sizeof("-Seg1-Frag") - 1;
		if (end_pos < start_pos ||
			ngx_memcmp(end_pos, "-Seg1-Frag", sizeof("-Seg1-Frag") - 1) != 0)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_hds_parse_uri_file_name: invalid segment / fragment requested");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		*request = &hds_fragment_request;
	}
	// bootstrap request
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hds.manifest_config.bootstrap_file_name_prefix, bootstrap_file_ext))
	{
		start_pos += conf->hds.manifest_config.bootstrap_file_name_prefix.len;
		end_pos -= (sizeof(bootstrap_file_ext) - 1);
		*request = &hds_bootstrap_request;
	}
	// manifest request
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hds.manifest_file_name_prefix, manifest_file_ext))
	{
		start_pos += conf->hds.manifest_file_name_prefix.len;
		end_pos -= (sizeof(manifest_file_ext) - 1);
		*request = &hds_manifest_request;
		flags = PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_hds_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, flags, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_hds_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hds_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	drm_info_t* result;
	ngx_int_t rc;
	
	rc = udrm_parse_response(
		&submodule_context->request_context,
		drm_info,
		FALSE,
		(void**)&result);
	if (rc != VOD_OK)
	{
		return NGX_ERROR;
	}

	if (result->pssh_array.count != 1)
	{
		ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_hds_parse_drm_info: pssh array must contain a single element");
		return NGX_ERROR;
	}

	*output = result;

	return NGX_OK;
}

DEFINE_SUBMODULE(hds);
