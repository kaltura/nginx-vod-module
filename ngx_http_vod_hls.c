#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/hls/hls_muxer.h"

// content types
static u_char mpeg_ts_content_type[] = "video/MP2T";
static u_char m3u8_content_type[] = "application/vnd.apple.mpegurl";
static u_char encryption_key_content_type[] = "application/octet-stream";

static const u_char ts_file_ext[] = ".ts";
static const u_char m3u8_file_ext[] = ".m3u8";
static const u_char key_file_ext[] = ".key";

// constants
static ngx_str_t empty_string = ngx_null_string;

static ngx_int_t
ngx_http_vod_hls_handle_master_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	if (submodule_context->conf->hls.absolute_master_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &empty_string, &base_url);
	}

	rc = m3u8_builder_build_master_playlist(
		&submodule_context->request_context,
		&submodule_context->conf->hls.m3u8_config,
		&base_url,
		submodule_context->request_params.uses_multi_uri,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_master_playlist: m3u8_builder_build_master_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = m3u8_content_type;
	content_type->len = sizeof(m3u8_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_hls_handle_index_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_str_t segments_base_url = ngx_null_string;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	if (submodule_context->conf->hls.absolute_index_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &submodule_context->r->uri, &base_url);

		ngx_http_vod_get_base_url(
			submodule_context->r, 
			&submodule_context->conf->https_header_name, 
			&submodule_context->conf->segments_base_url, 
			submodule_context->conf->segments_base_url_has_scheme, 
			&submodule_context->r->uri, 
			&segments_base_url);
	}

	rc = m3u8_builder_build_index_playlist(
		&submodule_context->request_context,
		&submodule_context->conf->hls.m3u8_config,
		&base_url,
		&segments_base_url,
		submodule_context->request_params.uses_multi_uri,
		submodule_context->conf->secret_key.len != 0,
		&submodule_context->conf->segmenter,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_index_playlist: m3u8_builder_build_index_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = m3u8_content_type;
	content_type->len = sizeof(m3u8_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_handle_iframe_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	mpeg_stream_metadata_t* cur_stream;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	for (cur_stream = submodule_context->mpeg_metadata.first_stream;
		cur_stream < submodule_context->mpeg_metadata.last_stream;
		cur_stream++)
	{
		if (cur_stream->media_info.media_type == MEDIA_TYPE_AUDIO &&
			cur_stream->media_info.speed_nom != cur_stream->media_info.speed_denom)
		{
			ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_handle_iframe_playlist: iframes playlist not supported with audio speed change");
			return NGX_HTTP_BAD_REQUEST;
		}
	}

	if (submodule_context->conf->hls.absolute_iframe_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &submodule_context->r->uri, &base_url);
	}

	rc = m3u8_builder_build_iframe_playlist(
		&submodule_context->request_context,
		&submodule_context->conf->hls.m3u8_config,
		&base_url,
		submodule_context->request_params.uses_multi_uri,
		&submodule_context->conf->segmenter,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: m3u8_builder_build_iframe_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = m3u8_content_type;
	content_type->len = sizeof(m3u8_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_handle_encryption_key(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	u_char* encryption_key;

	encryption_key = ngx_palloc(submodule_context->request_context.pool, BUFFER_CACHE_KEY_SIZE);
	if (encryption_key == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_encryption_key: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ngx_memcpy(encryption_key, submodule_context->request_params.suburis->file_key, BUFFER_CACHE_KEY_SIZE);

	response->data = encryption_key;
	response->len = BUFFER_CACHE_KEY_SIZE;

	content_type->data = encryption_key_content_type;
	content_type->len = sizeof(encryption_key_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	read_cache_state_t* read_cache_state,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	hls_muxer_state_t* state;
	vod_status_t rc;
	bool_t simulation_supported;

	state = ngx_pcalloc(submodule_context->request_context.pool, sizeof(hls_muxer_state_t));
	if (state == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_frame_processor: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	rc = hls_muxer_init(
		state,
		&submodule_context->request_context,
		submodule_context->request_params.segment_index,
		&submodule_context->mpeg_metadata,
		read_cache_state,
		segment_writer->write_tail,
		segment_writer->context,
		&simulation_supported);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_frame_processor: hls_muxer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (simulation_supported)
	{
		*response_size = hls_muxer_simulate_get_segment_size(state);
		hls_muxer_simulation_reset(state);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)hls_muxer_process;
	*frame_processor_state = state;

	content_type->len = sizeof(mpeg_ts_content_type) - 1;
	content_type->data = (u_char *)mpeg_ts_content_type;

	return NGX_OK;
}

static const ngx_http_vod_request_t hls_master_request = {
	0,
	PARSE_FLAG_TOTAL_SIZE_ESTIMATE | PARSE_FLAG_CODEC_NAME,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_hls_handle_master_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_index_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_BASIC_METADATA_ONLY,
	NULL,
	0,
	REQUEST_CLASS_MANIFEST,
	ngx_http_vod_hls_handle_index_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_iframes_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL_EXCEPT_OFFSETS | PARSE_FLAG_PARSED_EXTRA_DATA_SIZE,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_hls_handle_iframe_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_enc_key_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_BASIC_METADATA_ONLY,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_hls_handle_encryption_key,
	NULL,
};

static const ngx_http_vod_request_t hls_segment_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_hls_init_frame_processor,
};

void
ngx_http_vod_hls_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_hls_loc_conf_t *conf)
{
	conf->absolute_master_urls = NGX_CONF_UNSET;
	conf->absolute_index_urls = NGX_CONF_UNSET;
	conf->absolute_iframe_urls = NGX_CONF_UNSET;
}

static char *
ngx_http_vod_hls_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_hls_loc_conf_t *conf,
	ngx_http_vod_hls_loc_conf_t *prev)
{
	ngx_conf_merge_value(conf->absolute_master_urls, prev->absolute_master_urls, 1);
	ngx_conf_merge_value(conf->absolute_index_urls, prev->absolute_index_urls, 1);
	ngx_conf_merge_value(conf->absolute_iframe_urls, prev->absolute_iframe_urls, 0);

	ngx_conf_merge_str_value(conf->master_file_name_prefix, prev->master_file_name_prefix, "master");
	ngx_conf_merge_str_value(conf->m3u8_config.index_file_name_prefix, prev->m3u8_config.index_file_name_prefix, "index");	
	ngx_conf_merge_str_value(conf->iframes_file_name_prefix, prev->iframes_file_name_prefix, "iframes");
	ngx_conf_merge_str_value(conf->m3u8_config.segment_file_name_prefix, prev->m3u8_config.segment_file_name_prefix, "seg");
	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_file_name, prev->m3u8_config.encryption_key_file_name, "encryption");

	m3u8_builder_init_config(
		&conf->m3u8_config,
		base->segmenter.max_segment_duration);

	return NGX_CONF_OK;
}

static int 
ngx_http_vod_hls_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_hls_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	ngx_http_vod_request_params_t* request_params)
{
	bool_t expect_segment_index;
	ngx_int_t rc;

	// segment
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.segment_file_name_prefix, ts_file_ext))
	{
		start_pos += conf->hls.m3u8_config.segment_file_name_prefix.len;
		end_pos -= (sizeof(ts_file_ext) - 1);
		request_params->request = &hls_segment_request;
		expect_segment_index = TRUE;
	}
	// manifest
	else if (ngx_http_vod_ends_with_static(start_pos, end_pos, m3u8_file_ext))
	{
		end_pos -= (sizeof(m3u8_file_ext) - 1);

		// make sure the file name begins with 'index' or 'iframes'
		if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.m3u8_config.index_file_name_prefix))
		{
			request_params->request = &hls_index_request;
			start_pos += conf->hls.m3u8_config.index_file_name_prefix.len;
		}
		else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.iframes_file_name_prefix))
		{
			request_params->request = &hls_iframes_request;
			start_pos += conf->hls.iframes_file_name_prefix.len;
		}
		else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.master_file_name_prefix))
		{
			request_params->request = &hls_master_request;
			start_pos += conf->hls.master_file_name_prefix.len;
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_hls_parse_uri_file_name: unidentified m3u8 request");
			return NGX_HTTP_BAD_REQUEST;
		}

		expect_segment_index = FALSE;
	}
	// encryption key
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.encryption_key_file_name, key_file_ext))
	{
		start_pos += conf->hls.m3u8_config.encryption_key_file_name.len;
		end_pos -= (sizeof(key_file_ext)-1);
		request_params->request = &hls_enc_key_request;
		expect_segment_index = FALSE;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_hls_parse_uri_file_name: unidentified request");
		return NGX_HTTP_BAD_REQUEST;
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, expect_segment_index, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_hls_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_hls_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
		"ngx_http_vod_hls_parse_drm_info: drm support for hls not implemented");
	return VOD_UNEXPECTED;
}

DEFINE_SUBMODULE(hls);
