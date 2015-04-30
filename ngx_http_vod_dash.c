#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "ngx_http_vod_udrm.h"
#include "vod/dash/dash_packager.h"
#include "vod/dash/edash_packager.h"

// content types
static u_char mpd_content_type[] = "application/dash+xml";
static u_char mp4_audio_content_type[] = "audio/mp4";
static u_char mp4_video_content_type[] = "video/mp4";

// file extensions
static const u_char manifest_file_ext[] = ".mpd";
static const u_char init_segment_file_ext[] = ".mp4";
static const u_char fragment_file_ext[] = ".m4s";

static ngx_int_t 
ngx_http_vod_dash_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	if (submodule_context->conf->dash.absolute_manifest_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &submodule_context->r->uri, &base_url);
	}

	if (submodule_context->conf->drm_enabled)
	{
		rc = edash_packager_build_mpd(
			&submodule_context->request_context,
			&submodule_context->conf->dash.mpd_config,
			&base_url,
			&submodule_context->conf->segmenter,
			&submodule_context->mpeg_metadata,
			response);
	}
	else
	{
		rc = dash_packager_build_mpd(
			&submodule_context->request_context,
			&submodule_context->conf->dash.mpd_config,
			&base_url,
			&submodule_context->conf->segmenter,
			&submodule_context->mpeg_metadata,
			0,
			NULL,
			NULL,
			response);
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_handle_manifest: (e)dash_packager_build_mpd failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = mpd_content_type;
	content_type->len = sizeof(mpd_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_handle_init_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	if (submodule_context->conf->drm_enabled)
	{
		rc = edash_packager_build_init_mp4(
			&submodule_context->request_context,
			&submodule_context->mpeg_metadata,
			submodule_context->conf->drm_clear_lead_segment_count > 0,
			ngx_http_vod_submodule_size_only(submodule_context),
			response);
	}
	else
	{
		rc = dash_packager_build_init_mp4(
			&submodule_context->request_context,
			&submodule_context->mpeg_metadata,
			ngx_http_vod_submodule_size_only(submodule_context),
			NULL,
			NULL,
			response);
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_handle_init_segment: (e)dash_packager_build_init_mp4 failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (submodule_context->mpeg_metadata.stream_count[MEDIA_TYPE_VIDEO])
	{
		content_type->data = mp4_video_content_type;
		content_type->len = sizeof(mp4_video_content_type) - 1;
	}
	else
	{
		content_type->data = mp4_audio_content_type;
		content_type->len = sizeof(mp4_audio_content_type) - 1;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	read_cache_state_t* read_cache_state,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	fragment_writer_state_t* state;
	segment_writer_t edash_writer;
	vod_status_t rc;
	bool_t size_only = ngx_http_vod_submodule_size_only(submodule_context);

	if (submodule_context->conf->drm_enabled && 
		submodule_context->request_params.segment_index >= submodule_context->conf->drm_clear_lead_segment_count)
	{
		rc = edash_packager_get_fragment_writer(
			&edash_writer,
			&submodule_context->request_context,
			submodule_context->mpeg_metadata.first_stream,
			submodule_context->request_params.segment_index,
			segment_writer,
			submodule_context->request_params.suburis->file_key,		// iv
			size_only,
			output_buffer,
			response_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_init_frame_processor: edash_packager_get_fragment_writer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		segment_writer = &edash_writer;
	}
	else
	{
		// build the fragment header
		rc = dash_packager_build_fragment_header(
			&submodule_context->request_context,
			submodule_context->mpeg_metadata.first_stream,
			submodule_context->request_params.segment_index,
			submodule_context->conf->drm_enabled ? 2 : 0,
			0,
			NULL,
			NULL,
			NULL,
			size_only,
			output_buffer,
			response_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_init_frame_processor: dash_packager_build_fragment_header failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}
	}

	// initialize the frame processor
	if (!size_only || *response_size == 0)
	{
		rc = mp4_builder_frame_writer_init(
			&submodule_context->request_context,
			submodule_context->mpeg_metadata.first_stream,
			read_cache_state,
			segment_writer->write_tail,
			segment_writer->context,
			&state);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_init_frame_processor: mp4_builder_frame_writer_init failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		*frame_processor = (ngx_http_vod_frame_processor_t)mp4_builder_frame_writer_process;
		*frame_processor_state = state;
	}

	// set the 'Content-type' header
	if (submodule_context->mpeg_metadata.stream_count[MEDIA_TYPE_VIDEO])
	{
		content_type->len = sizeof(mp4_video_content_type) - 1;
		content_type->data = (u_char *)mp4_video_content_type;
	}
	else
	{
		content_type->len = sizeof(mp4_audio_content_type) - 1;
		content_type->data = (u_char *)mp4_audio_content_type;
	}

	return NGX_OK;
}

static const ngx_http_vod_request_t dash_manifest_request = {
	0,
	PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE | PARSE_FLAG_CODEC_NAME,
	dash_packager_compare_streams,
	offsetof(ngx_http_vod_loc_conf_t, duplicate_bitrate_threshold),
	REQUEST_CLASS_MANIFEST,
	ngx_http_vod_dash_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t dash_init_request = {
	REQUEST_FLAG_SINGLE_STREAM,
	PARSE_BASIC_METADATA_ONLY | PARSE_FLAG_SAVE_RAW_ATOMS,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_dash_handle_init_segment,
	NULL,
};

static const ngx_http_vod_request_t dash_fragment_request = {
	REQUEST_FLAG_SINGLE_STREAM,
	PARSE_FLAG_FRAMES_ALL,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_dash_init_frame_processor,
};

static const ngx_http_vod_request_t edash_fragment_request = {
	REQUEST_FLAG_SINGLE_STREAM,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_dash_init_frame_processor,
};

static void
ngx_http_vod_dash_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_dash_loc_conf_t *conf)
{
	conf->absolute_manifest_urls = NGX_CONF_UNSET;
	conf->mpd_config.segment_timeline = NGX_CONF_UNSET;
}

static char *
ngx_http_vod_dash_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_dash_loc_conf_t *conf,
	ngx_http_vod_dash_loc_conf_t *prev)
{
	ngx_conf_merge_value(conf->absolute_manifest_urls, prev->absolute_manifest_urls, 1);

	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");
	ngx_conf_merge_str_value(conf->mpd_config.init_file_name_prefix, prev->mpd_config.init_file_name_prefix, "init");
	ngx_conf_merge_str_value(conf->mpd_config.fragment_file_name_prefix, prev->mpd_config.fragment_file_name_prefix, "fragment");
	ngx_conf_merge_value(conf->mpd_config.segment_timeline, prev->mpd_config.segment_timeline, 1);

	return NGX_CONF_OK;
}

static int
ngx_http_vod_dash_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_dash_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	ngx_http_vod_request_params_t* request_params)
{
	ngx_int_t rc;
	bool_t expect_segment_index;

	// fragment
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.fragment_file_name_prefix, fragment_file_ext))
	{
		start_pos += conf->dash.mpd_config.fragment_file_name_prefix.len;
		end_pos -= (sizeof(fragment_file_ext) - 1);
		request_params->request = conf->drm_enabled ? &edash_fragment_request : &dash_fragment_request;
		expect_segment_index = TRUE;
	}
	// init segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.init_file_name_prefix, init_segment_file_ext))
	{
		start_pos += conf->dash.mpd_config.init_file_name_prefix.len;
		end_pos -= (sizeof(init_segment_file_ext) - 1);
		request_params->request = &dash_init_request;
		expect_segment_index = FALSE;
	}
	// manifest
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.manifest_file_name_prefix, manifest_file_ext))
	{
		start_pos += conf->dash.manifest_file_name_prefix.len;
		end_pos -= (sizeof(manifest_file_ext) - 1);
		request_params->request = &dash_manifest_request;
		expect_segment_index = FALSE;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_dash_parse_uri_file_name: unidentified request");
		return NGX_HTTP_BAD_REQUEST;
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, expect_segment_index, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_dash_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	return 	ngx_http_vod_udrm_parse_response(
		&submodule_context->request_context,
		drm_info,
		output);
}

DEFINE_SUBMODULE(dash);
