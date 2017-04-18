#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/subtitle/webvtt_builder.h"
#include "vod/hls/hls_muxer.h"
#include "vod/udrm.h"

// constants
#define SUPPORTED_CODECS \
	(VOD_CODEC_FLAG(AVC) | \
	VOD_CODEC_FLAG(HEVC) | \
	VOD_CODEC_FLAG(AAC) | \
	VOD_CODEC_FLAG(AC3) | \
	VOD_CODEC_FLAG(EAC3) | \
	VOD_CODEC_FLAG(MP3))

// content types
static u_char m3u8_content_type[] = "application/vnd.apple.mpegurl";
static u_char encryption_key_content_type[] = "application/octet-stream";
static u_char mpeg_ts_content_type[] = "video/MP2T";
static u_char vtt_content_type[] = "text/vtt";

static const u_char ts_file_ext[] = ".ts";
static const u_char vtt_file_ext[] = ".vtt";
static const u_char m3u8_file_ext[] = ".m3u8";
static const u_char key_file_ext[] = ".key";

// constants
static ngx_str_t empty_string = ngx_null_string;

ngx_conf_enum_t  hls_encryption_methods[] = {
	{ ngx_string("none"), HLS_ENC_NONE },
	{ ngx_string("aes-128"), HLS_ENC_AES_128 },
	{ ngx_string("sample-aes"), HLS_ENC_SAMPLE_AES },
	{ ngx_null_string, 0 }
};

static void
ngx_http_vod_hls_init_encryption_iv(u_char* iv, uint32_t segment_index)
{
	u_char* p;

	// the IV is the segment index in big endian
	vod_memzero(iv, AES_BLOCK_SIZE - sizeof(uint32_t));
	segment_index++;
	p = iv + AES_BLOCK_SIZE - sizeof(uint32_t);
	*p++ = (u_char)(segment_index >> 24);
	*p++ = (u_char)(segment_index >> 16);
	*p++ = (u_char)(segment_index >> 8);
	*p++ = (u_char)(segment_index);
}

static void
ngx_http_vod_hls_init_encryption_params(
	hls_encryption_params_t* encryption_params,
	ngx_http_vod_submodule_context_t* submodule_context,
	u_char* iv)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	drm_info_t* drm_info;

	encryption_params->type = conf->hls.encryption_method;
	if (encryption_params->type == HLS_ENC_NONE)
	{
		return;
	}

	encryption_params->iv = iv;

	if (conf->drm_enabled)
	{
		drm_info = submodule_context->media_set.sequences[0].drm_info;
		encryption_params->key = drm_info->key;

		if (drm_info->iv_set)
		{
			encryption_params->iv = drm_info->iv;
		}
	}
	else
	{
		encryption_params->key = submodule_context->media_set.sequences[0].encryption_key;
	}

	if (encryption_params->iv == iv)
	{
		ngx_http_vod_hls_init_encryption_iv(iv, submodule_context->request_params.segment_index);
	}
}

static ngx_int_t
ngx_http_vod_hls_handle_master_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	if (conf->hls.absolute_master_urls)
	{
		rc = ngx_http_vod_get_base_url(submodule_context->r, conf->base_url, &empty_string, &base_url);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	rc = m3u8_builder_build_master_playlist(
		&submodule_context->request_context,
		&conf->hls.m3u8_config,
		conf->hls.encryption_method,
		&base_url,
		&submodule_context->media_set,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_master_playlist: m3u8_builder_build_master_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
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
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	hls_encryption_params_t encryption_params;
	ngx_str_t segments_base_url = ngx_null_string;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	u_char iv[AES_BLOCK_SIZE];

	if (conf->hls.absolute_index_urls)
	{
		rc = ngx_http_vod_get_base_url(submodule_context->r, conf->base_url, &submodule_context->r->uri, &base_url);
		if (rc != NGX_OK)
		{
			return rc;
		}

		if (conf->segments_base_url != NULL)
		{
			rc = ngx_http_vod_get_base_url(
				submodule_context->r, 
				conf->segments_base_url,
				&submodule_context->r->uri, 
				&segments_base_url);
			if (rc != NGX_OK)
			{
				return rc;
			}
		}
		else
		{
			segments_base_url = base_url;
		}
	}

	ngx_http_vod_hls_init_encryption_params(&encryption_params, submodule_context, iv);

	if (encryption_params.type != HLS_ENC_NONE)
	{
		if (conf->hls.encryption_key_uri != NULL)
		{
			if (ngx_http_complex_value(
				submodule_context->r,
				conf->hls.encryption_key_uri,
				&encryption_params.key_uri) != NGX_OK)
			{
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
					"ngx_http_vod_hls_handle_index_playlist: ngx_http_complex_value failed");
				return NGX_ERROR;
			}
		}
		else
		{
			encryption_params.key_uri.len = 0;
		}
	}

	rc = m3u8_builder_build_index_playlist(
		&submodule_context->request_context,
		&conf->hls.m3u8_config,
		&base_url,
		&segments_base_url,
		&submodule_context->request_params,
		&encryption_params,
		&submodule_context->media_set,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_index_playlist: m3u8_builder_build_index_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
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
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	
	if (conf->hls.encryption_method != HLS_ENC_NONE)
	{
		ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: iframes playlist not supported with encryption");
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
	}

	if (submodule_context->media_set.audio_filtering_needed)
	{
		ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: iframes playlist not supported with audio filtering");
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
	}

	if (conf->hls.absolute_iframe_urls)
	{
		rc = ngx_http_vod_get_base_url(submodule_context->r, conf->base_url, &submodule_context->r->uri, &base_url);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

	rc = m3u8_builder_build_iframe_playlist(
		&submodule_context->request_context,
		&conf->hls.m3u8_config,
		&conf->hls.muxer_config,
		&base_url,
		&submodule_context->request_params,
		&submodule_context->media_set,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: m3u8_builder_build_iframe_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
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
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_ALLOC_FAILED);
	}

	ngx_memcpy(encryption_key, submodule_context->media_set.sequences[0].encryption_key, BUFFER_CACHE_KEY_SIZE);

	response->data = encryption_key;
	response->len = BUFFER_CACHE_KEY_SIZE;

	content_type->data = encryption_key_content_type;
	content_type->len = sizeof(encryption_key_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_ts_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	hls_encryption_params_t encryption_params;
	hls_muxer_state_t* state;
	vod_status_t rc;
	u_char iv[AES_BLOCK_SIZE];

	ngx_http_vod_hls_init_encryption_params(&encryption_params, submodule_context, iv);

	rc = hls_muxer_init_segment(
		&submodule_context->request_context,
		&submodule_context->conf->hls.muxer_config,
		&encryption_params,
		submodule_context->request_params.segment_index,
		&submodule_context->media_set,
		segment_writer->write_tail,
		segment_writer->context,
		response_size, 
		output_buffer,
		&state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_ts_frame_processor: hls_muxer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)hls_muxer_process;
	*frame_processor_state = state;

	content_type->len = sizeof(mpeg_ts_content_type) - 1;
	content_type->data = (u_char *)mpeg_ts_content_type;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_handle_vtt_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;
	
	rc = webvtt_builder_build(
		&submodule_context->request_context,
		&submodule_context->media_set,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_vtt_segment: webvtt_builder_build failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	content_type->len = sizeof(vtt_content_type) - 1;
	content_type->data = (u_char *)vtt_content_type;

	return NGX_OK;
}

static const ngx_http_vod_request_t hls_master_request = {
	0,
	PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE | PARSE_FLAG_KEY_FRAME_BITRATE | PARSE_FLAG_CODEC_NAME | PARSE_FLAG_PARSED_EXTRA_DATA_SIZE,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS | VOD_CODEC_FLAG(WEBVTT),
	HLS_TIMESCALE,
	ngx_http_vod_hls_handle_master_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_index_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE | REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE,
	PARSE_BASIC_METADATA_ONLY,
	REQUEST_CLASS_MANIFEST,
	SUPPORTED_CODECS | VOD_CODEC_FLAG(WEBVTT),
	HLS_TIMESCALE,
	ngx_http_vod_hls_handle_index_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_iframes_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE | REQUEST_FLAG_PARSE_ALL_CLIPS,
	PARSE_FLAG_FRAMES_ALL_EXCEPT_OFFSETS | PARSE_FLAG_PARSED_EXTRA_DATA_SIZE,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS,
	HLS_TIMESCALE,
	ngx_http_vod_hls_handle_iframe_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_enc_key_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_BASIC_METADATA_ONLY,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS,
	HLS_TIMESCALE,
	ngx_http_vod_hls_handle_encryption_key,
	NULL,
};

static const ngx_http_vod_request_t hls_ts_segment_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS,
	HLS_TIMESCALE,
	NULL,
	ngx_http_vod_hls_init_ts_frame_processor,
};

static const ngx_http_vod_request_t hls_vtt_segment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	VOD_CODEC_FLAG(WEBVTT),
	WEBVTT_TIMESCALE,
	ngx_http_vod_hls_handle_vtt_segment,
	NULL,
};

static void
ngx_http_vod_hls_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_hls_loc_conf_t *conf)
{
	conf->absolute_master_urls = NGX_CONF_UNSET;
	conf->absolute_index_urls = NGX_CONF_UNSET;
	conf->absolute_iframe_urls = NGX_CONF_UNSET;
	conf->muxer_config.interleave_frames = NGX_CONF_UNSET;
	conf->muxer_config.align_frames = NGX_CONF_UNSET;
	conf->muxer_config.output_id3_timestamps = NGX_CONF_UNSET;
	conf->encryption_method = NGX_CONF_UNSET_UINT;
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
	ngx_conf_merge_str_value(conf->m3u8_config.iframes_file_name_prefix, prev->m3u8_config.iframes_file_name_prefix, "iframes");
	ngx_conf_merge_str_value(conf->m3u8_config.segment_file_name_prefix, prev->m3u8_config.segment_file_name_prefix, "seg");

	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_file_name, prev->m3u8_config.encryption_key_file_name, "encryption");
	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_format, prev->m3u8_config.encryption_key_format, "");
	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_format_versions, prev->m3u8_config.encryption_key_format_versions, "");
	if (conf->encryption_key_uri == NULL)
	{
		conf->encryption_key_uri = prev->encryption_key_uri;
	}

	ngx_conf_merge_value(conf->muxer_config.interleave_frames, prev->muxer_config.interleave_frames, 0);
	ngx_conf_merge_value(conf->muxer_config.align_frames, prev->muxer_config.align_frames, 1);
	ngx_conf_merge_value(conf->muxer_config.output_id3_timestamps, prev->muxer_config.output_id3_timestamps, 0);
	
	ngx_conf_merge_uint_value(conf->encryption_method, prev->encryption_method, HLS_ENC_NONE);

	m3u8_builder_init_config(
		&conf->m3u8_config,
		base->segmenter.max_segment_duration, 
		conf->encryption_method);

	if (conf->encryption_method != HLS_ENC_NONE &&
		base->secret_key == NULL &&
		!base->drm_enabled)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_secret_key\" must be set when \"vod_hls_encryption_method\" is not none");
		return NGX_CONF_ERROR;
	}

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
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	uint32_t flags;
	ngx_int_t rc;

	// ts segment
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.segment_file_name_prefix, ts_file_ext))
	{
		start_pos += conf->hls.m3u8_config.segment_file_name_prefix.len;
		end_pos -= (sizeof(ts_file_ext) - 1);
		*request = &hls_ts_segment_request;
		flags = PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX;
	}
	// vtt segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.segment_file_name_prefix, vtt_file_ext))
	{
		start_pos += conf->hls.m3u8_config.segment_file_name_prefix.len;
		end_pos -= (sizeof(vtt_file_ext) - 1);
		*request = &hls_vtt_segment_request;
		flags = PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX;
	}
	// manifest
	else if (ngx_http_vod_ends_with_static(start_pos, end_pos, m3u8_file_ext))
	{
		end_pos -= (sizeof(m3u8_file_ext) - 1);

		// make sure the file name begins with 'index' or 'iframes'
		if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.m3u8_config.index_file_name_prefix))
		{
			*request = &hls_index_request;
			start_pos += conf->hls.m3u8_config.index_file_name_prefix.len;
			flags = 0;
		}
		else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.m3u8_config.iframes_file_name_prefix))
		{
			*request = &hls_iframes_request;
			start_pos += conf->hls.m3u8_config.iframes_file_name_prefix.len;
			flags = 0;
		}
		else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.master_file_name_prefix))
		{
			*request = &hls_master_request;
			start_pos += conf->hls.master_file_name_prefix.len;
			flags = PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE;
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_hls_parse_uri_file_name: unidentified m3u8 request");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}
	}
	// encryption key
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.encryption_key_file_name, key_file_ext) &&
		!conf->drm_enabled)
	{
		start_pos += conf->hls.m3u8_config.encryption_key_file_name.len;
		end_pos -= (sizeof(key_file_ext) - 1);
		*request = &hls_enc_key_request;
		flags = 0;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_hls_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, flags, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_hls_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	vod_status_t rc;

	rc = udrm_parse_response(
		&submodule_context->request_context,
		drm_info,
		TRUE,
		output);
	if (rc != VOD_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}

DEFINE_SUBMODULE(hls);
