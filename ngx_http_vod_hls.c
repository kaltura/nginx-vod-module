#include <ngx_http.h>
#include <ngx_md5.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/subtitle/webvtt_builder.h"
#include "vod/hls/hls_muxer.h"
#include "vod/mp4/mp4_muxer.h"
#include "vod/mp4/mp4_fragment.h"
#include "vod/mp4/mp4_init_segment.h"
#include "vod/udrm.h"

#if (NGX_HAVE_OPENSSL_EVP)
#include "vod/dash/edash_packager.h"
#include "vod/mp4/mp4_cbcs_encrypt.h"
#include "vod/hls/aes_cbc_encrypt.h"
#endif // NGX_HAVE_OPENSSL_EVP

// constants
#define SUPPORTED_CODECS_MP4 \
	(VOD_CODEC_FLAG(AVC) | \
	VOD_CODEC_FLAG(HEVC) | \
	VOD_CODEC_FLAG(AV1) | \
	VOD_CODEC_FLAG(AAC) | \
	VOD_CODEC_FLAG(AC3) | \
	VOD_CODEC_FLAG(EAC3) | \
	VOD_CODEC_FLAG(MP3) | \
	VOD_CODEC_FLAG(DTS) | \
	VOD_CODEC_FLAG(FLAC))

#define SUPPORTED_CODECS_TS \
	(VOD_CODEC_FLAG(AVC) | \
	VOD_CODEC_FLAG(HEVC) | \
	VOD_CODEC_FLAG(AAC) | \
	VOD_CODEC_FLAG(AC3) | \
	VOD_CODEC_FLAG(EAC3) | \
	VOD_CODEC_FLAG(MP3) | \
	VOD_CODEC_FLAG(DTS))

#define SUPPORTED_CODECS (SUPPORTED_CODECS_MP4 | SUPPORTED_CODECS_TS)

#define ID3_TEXT_JSON_FORMAT "{\"timestamp\":%uL}%Z"
#define ID3_TEXT_JSON_SEQUENCE_ID_PREFIX_FORMAT "{\"timestamp\":%uL,\"sequenceId\":\""
#define ID3_TEXT_JSON_SEQUENCE_ID_SUFFIX "\"}"


// content types
static u_char m3u8_content_type[] = "application/vnd.apple.mpegurl";
static u_char encryption_key_content_type[] = "application/octet-stream";
static u_char mpeg_ts_content_type[] = "video/MP2T";
static u_char vtt_content_type[] = "text/vtt";

static const u_char ts_file_ext[] = ".ts";
static const u_char m4s_file_ext[] = ".m4s";
static const u_char vtt_file_ext[] = ".vtt";
static const u_char mp4_file_ext[] = ".mp4";
static const u_char m3u8_file_ext[] = ".m3u8";
static const u_char key_file_ext[] = ".key";

// constants
static ngx_str_t empty_string = ngx_null_string;

ngx_conf_enum_t  hls_encryption_methods[] = {
	{ ngx_string("none"), HLS_ENC_NONE },
	{ ngx_string("aes-128"), HLS_ENC_AES_128 },
	{ ngx_string("sample-aes"), HLS_ENC_SAMPLE_AES },
	{ ngx_string("sample-aes-cenc"), HLS_ENC_SAMPLE_AES_CENC },
	{ ngx_null_string, 0 }
};

ngx_conf_enum_t  hls_container_formats[] = {
	{ ngx_string("auto"), HLS_CONTAINER_AUTO },
	{ ngx_string("mpegts"), HLS_CONTAINER_MPEGTS },
	{ ngx_string("fmp4"), HLS_CONTAINER_FMP4 },
	{ ngx_null_string, 0 }
};

static ngx_uint_t
ngx_http_vod_hls_get_container_format(
	ngx_http_vod_hls_loc_conf_t* conf,
	media_set_t* media_set)
{
	media_track_t* track;

	if (conf->m3u8_config.container_format != HLS_CONTAINER_AUTO)
	{
		return conf->m3u8_config.container_format;
	}

	track = media_set->filtered_tracks;
	if ((track->media_info.media_type == MEDIA_TYPE_VIDEO && track->media_info.codec_id != VOD_CODEC_ID_AVC) ||
		conf->encryption_method == HLS_ENC_SAMPLE_AES_CENC)
	{
		return HLS_CONTAINER_FMP4;
	}

	return HLS_CONTAINER_MPEGTS;
}

#if (NGX_HAVE_OPENSSL_EVP)
// some random salt to prevent the iv from being equal to key in case encryption_iv_seed is null
static u_char iv_salt[] = { 0xa7, 0xc6, 0x17, 0xab, 0x52, 0x2c, 0x40, 0x3c, 0xf6, 0x8a };

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

static ngx_int_t
ngx_http_vod_hls_get_iv_seed(
	ngx_http_vod_submodule_context_t* submodule_context, 
	media_sequence_t* sequence,
	ngx_str_t* result)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	ngx_http_complex_value_t* value;

	if (conf->encryption_iv_seed != NULL)
	{
		value = conf->encryption_iv_seed;
	}
	else if (conf->secret_key != NULL)
	{
		value = conf->secret_key;
	}
	else
	{
		*result = sequence->mapped_uri;
		return NGX_OK;
	}

	if (ngx_http_complex_value(
		submodule_context->r,
		value,
		result) != NGX_OK)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_get_iv_seed: ngx_http_complex_value failed");
		return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_encryption_params(
	hls_encryption_params_t* encryption_params,
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_uint_t container_format)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	media_sequence_t* sequence;
	drm_info_t* drm_info;
	ngx_str_t iv_seed;
	ngx_md5_t md5;
	ngx_int_t rc;

	encryption_params->type = conf->hls.encryption_method;
	if (encryption_params->type == HLS_ENC_NONE)
	{
		return NGX_OK;
	}

	encryption_params->iv = encryption_params->iv_buf;
	encryption_params->return_iv = conf->hls.output_iv;

	sequence = &submodule_context->media_set.sequences[0];

	if (conf->drm_enabled)
	{
		drm_info = sequence->drm_info;
		encryption_params->key = drm_info->key;

		if (drm_info->iv_set)
		{
			encryption_params->iv = drm_info->iv;
			return NGX_OK;
		}
	}
	else
	{
		encryption_params->key = sequence->encryption_key;
	}

	if (container_format != HLS_CONTAINER_FMP4 || encryption_params->type != HLS_ENC_AES_128)
	{
		ngx_http_vod_hls_init_encryption_iv(
			encryption_params->iv_buf,
			submodule_context->request_params.segment_index);
		return NGX_OK;
	}

	// must generate an iv in this case
	rc = ngx_http_vod_hls_get_iv_seed(
		submodule_context,
		sequence,
		&iv_seed);
	if (rc != NGX_OK)
	{
		return rc;
	}

	ngx_md5_init(&md5);
	ngx_md5_update(&md5, iv_salt, sizeof(iv_salt));
	ngx_md5_update(&md5, iv_seed.data, iv_seed.len);
	ngx_md5_final(encryption_params->iv_buf, &md5);
	encryption_params->return_iv = TRUE;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_segment_encryption(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_uint_t container_format,
	hls_encryption_params_t* encryption_params)
{
	aes_cbc_encrypt_context_t* encrypted_write_context;
	buffer_pool_t* buffer_pool;
	vod_status_t rc;

	rc = ngx_http_vod_hls_init_encryption_params(encryption_params, submodule_context, container_format);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (encryption_params->type != HLS_ENC_AES_128)
	{
		return NGX_OK;
	}

	if (container_format == HLS_CONTAINER_MPEGTS)
	{
		buffer_pool = submodule_context->request_context.output_buffer_pool;
	}
	else
	{
		// Note: should not use buffer pool for fmp4 since the buffers have varying sizes
		buffer_pool = NULL;
	}

	rc = aes_cbc_encrypt_init(
		&encrypted_write_context,
		&submodule_context->request_context,
		segment_writer->write_tail,
		segment_writer->context,
		buffer_pool,
		encryption_params->key,
		encryption_params->iv);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_segment_encryption: aes_cbc_encrypt_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	segment_writer->write_tail = (write_callback_t)aes_cbc_encrypt_write;
	segment_writer->context = encrypted_write_context;
	return NGX_OK;
}
#endif // NGX_HAVE_OPENSSL_EVP

static ngx_int_t
ngx_http_vod_hls_get_default_id3_data(ngx_http_vod_submodule_context_t* submodule_context, ngx_str_t* id3_data)
{
	media_set_t* media_set;
	vod_str_t* sequence_id;
	int64_t timestamp;
	u_char* p;
	size_t sequence_id_escape;
	size_t data_size;

	media_set = &submodule_context->media_set;
	sequence_id = &media_set->sequences[0].id;
	if (sequence_id->len != 0)
	{
		sequence_id_escape = vod_escape_json(NULL, sequence_id->data, sequence_id->len);
		data_size = sizeof(ID3_TEXT_JSON_SEQUENCE_ID_PREFIX_FORMAT) + VOD_INT64_LEN +
			sequence_id->len + sequence_id_escape +
			sizeof(ID3_TEXT_JSON_SEQUENCE_ID_SUFFIX);
	}
	else
	{
		sequence_id_escape = 0;
		data_size = sizeof(ID3_TEXT_JSON_FORMAT) + VOD_INT64_LEN;
	}

	timestamp = media_set_get_segment_time_millis(media_set);

	p = ngx_pnalloc(submodule_context->request_context.pool, data_size);
	if (p == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_get_default_id3_data: ngx_pnalloc failed");
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_ALLOC_FAILED);
	}

	id3_data->data = p;

	if (sequence_id->len != 0)
	{
		p = vod_sprintf(p, ID3_TEXT_JSON_SEQUENCE_ID_PREFIX_FORMAT, timestamp);
		if (sequence_id_escape)
		{
			p = (u_char*)vod_escape_json(p, sequence_id->data, sequence_id->len);
		}
		else
		{
			p = vod_copy(p, sequence_id->data, sequence_id->len);
		}
		p = vod_copy(p, ID3_TEXT_JSON_SEQUENCE_ID_SUFFIX, sizeof(ID3_TEXT_JSON_SEQUENCE_ID_SUFFIX));

	}
	else
	{
		p = vod_sprintf(p, ID3_TEXT_JSON_FORMAT, timestamp);
	}

	id3_data->len = p - id3_data->data;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_muxer_conf(ngx_http_vod_submodule_context_t* submodule_context, hls_mpegts_muxer_conf_t* conf)
{
	ngx_http_vod_hls_loc_conf_t* hls_conf;

	hls_conf = &submodule_context->conf->hls;

	conf->interleave_frames = hls_conf->interleave_frames;
	conf->align_frames = hls_conf->align_frames;
	conf->align_pts = hls_conf->align_pts;

	if (!hls_conf->output_id3_timestamps)
	{
		conf->id3_data.data = NULL;
		conf->id3_data.len = 0;
		return NGX_OK;
	}

	if (hls_conf->id3_data != NULL)
	{
		if (ngx_http_complex_value(
			submodule_context->r,
			hls_conf->id3_data,
			&conf->id3_data) != NGX_OK)
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_init_muxer_conf: ngx_http_complex_value failed");
			return NGX_ERROR;
		}

		return NGX_OK;
	}

	return ngx_http_vod_hls_get_default_id3_data(submodule_context, &conf->id3_data);
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
	ngx_uint_t container_format;
	ngx_str_t segments_base_url = ngx_null_string;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

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

	container_format = ngx_http_vod_hls_get_container_format(
		&conf->hls, 
		&submodule_context->media_set);

#if (NGX_HAVE_OPENSSL_EVP)
	rc = ngx_http_vod_hls_init_encryption_params(&encryption_params, submodule_context, container_format);
	if (rc != NGX_OK)
	{
		return rc;
	}

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
#else
	encryption_params.type = HLS_ENC_NONE;
#endif // NGX_HAVE_OPENSSL_EVP

	rc = m3u8_builder_build_index_playlist(
		&submodule_context->request_context,
		&conf->hls.m3u8_config,
		&base_url,
		&segments_base_url,
		&encryption_params,
		container_format,
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
	hls_mpegts_muxer_conf_t muxer_conf;
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

	if (ngx_http_vod_hls_get_container_format(
		&conf->hls,
		&submodule_context->media_set) == HLS_CONTAINER_FMP4)
	{
		ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: iframes playlist not supported with fmp4 container");
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
	}

	rc = ngx_http_vod_hls_init_muxer_conf(submodule_context, &muxer_conf);
	if (rc != NGX_OK)
	{
		return rc;
	}

	rc = m3u8_builder_build_iframe_playlist(
		&submodule_context->request_context,
		&conf->hls.m3u8_config,
		&muxer_conf,
		&base_url,
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
	hls_mpegts_muxer_conf_t muxer_conf;
	hls_muxer_state_t* state;
	vod_status_t rc;
	bool_t reuse_output_buffers;

#if (NGX_HAVE_OPENSSL_EVP)
	rc = ngx_http_vod_hls_init_segment_encryption(
		submodule_context,
		segment_writer,
		HLS_CONTAINER_MPEGTS,
		&encryption_params);
	if (rc != NGX_OK)
	{
		return rc;
	}

	if (encryption_params.type == HLS_ENC_SAMPLE_AES_CENC)
	{
		ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_ts_frame_processor: sample aes cenc not supported with mpeg ts container");
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
	}

	reuse_output_buffers = encryption_params.type == HLS_ENC_AES_128;
#else
	encryption_params.type = HLS_ENC_NONE;
	reuse_output_buffers = FALSE;
#endif // NGX_HAVE_OPENSSL_EVP

	rc = ngx_http_vod_hls_init_muxer_conf(submodule_context, &muxer_conf);
	if (rc != NGX_OK)
	{
		return rc;
	}

	rc = hls_muxer_init_segment(
		&submodule_context->request_context,
		&muxer_conf,
		&encryption_params,
		submodule_context->request_params.segment_index,
		&submodule_context->media_set,
		segment_writer->write_tail,
		segment_writer->context,
		reuse_output_buffers,
		response_size, 
		output_buffer,
		&state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_ts_frame_processor: hls_muxer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	if (encryption_params.type == HLS_ENC_AES_128 && 
		*response_size != 0)
	{
		*response_size = aes_round_up_to_block(*response_size);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)hls_muxer_process;
	*frame_processor_state = state;

	content_type->len = sizeof(mpeg_ts_content_type) - 1;
	content_type->data = (u_char *)mpeg_ts_content_type;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_handle_mp4_init_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	atom_writer_t* stsd_atom_writers = NULL;
	vod_status_t rc;

#if (NGX_HAVE_OPENSSL_EVP)
	aes_cbc_encrypt_context_t* encrypted_write_context;
	hls_encryption_params_t encryption_params;
	drm_info_t* drm_info;

	rc = ngx_http_vod_hls_init_encryption_params(&encryption_params, submodule_context, HLS_CONTAINER_FMP4);
	if (rc != NGX_OK)
	{
		return rc;
	}

	switch (encryption_params.type)
	{
	case HLS_ENC_SAMPLE_AES:
		rc = mp4_init_segment_get_encrypted_stsd_writers(
			&submodule_context->request_context,
			&submodule_context->media_set,
			SCHEME_TYPE_CBCS,
			FALSE,
			NULL,
			encryption_params.iv,
			&stsd_atom_writers);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_handle_mp4_init_segment: mp4_init_segment_get_encrypted_stsd_writers failed %i (1)", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
		break;

	case HLS_ENC_SAMPLE_AES_CENC:
		drm_info = (drm_info_t*)submodule_context->media_set.sequences[0].drm_info;

		rc = mp4_init_segment_get_encrypted_stsd_writers(
			&submodule_context->request_context,
			&submodule_context->media_set,
			SCHEME_TYPE_CENC,
			FALSE,
			drm_info->key_id,
			NULL,
			&stsd_atom_writers);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_handle_mp4_init_segment: mp4_init_segment_get_encrypted_stsd_writers failed %i (2)", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
		break;

	default:;
	}
#endif // NGX_HAVE_OPENSSL_EVP

	rc = mp4_init_segment_build(
		&submodule_context->request_context,
		&submodule_context->media_set,
		ngx_http_vod_submodule_size_only(submodule_context),
		NULL,
		stsd_atom_writers,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_mp4_init_segment: mp4_init_segment_build failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

#if (NGX_HAVE_OPENSSL_EVP)
	if (encryption_params.type == HLS_ENC_AES_128)
	{
		rc = aes_cbc_encrypt_init(
			&encrypted_write_context,
			&submodule_context->request_context,
			NULL,
			NULL,
			NULL,
			encryption_params.key,
			encryption_params.iv);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_handle_mp4_init_segment: aes_cbc_encrypt_init failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}

		rc = aes_cbc_encrypt(
			encrypted_write_context,
			response,
			response,
			TRUE);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_handle_mp4_init_segment: aes_cbc_encrypt failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
	}
#endif // NGX_HAVE_OPENSSL_EVP

	mp4_fragment_get_content_type(
		submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO],
		content_type);
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_fmp4_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	dash_fragment_header_extensions_t header_extensions;
	fragment_writer_state_t* state;
	mp4_muxer_state_t* muxer_state;
	segment_writer_t* segment_writers;
	vod_status_t rc;
	bool_t per_stream_writer;
	bool_t reuse_input_buffers = FALSE;
	bool_t size_only = ngx_http_vod_submodule_size_only(submodule_context);

#if (NGX_HAVE_OPENSSL_EVP)
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	hls_encryption_params_t encryption_params;
	segment_writer_t drm_writer;

	rc = ngx_http_vod_hls_init_segment_encryption(
		submodule_context,
		segment_writer,
		HLS_CONTAINER_FMP4,
		&encryption_params);
	if (rc != NGX_OK)
	{
		return rc;
	}

	reuse_input_buffers = encryption_params.type != HLS_ENC_NONE;

	if (conf->hls.encryption_method == HLS_ENC_SAMPLE_AES)
	{
		rc = mp4_cbcs_encrypt_get_writers(
			&submodule_context->request_context,
			&submodule_context->media_set,
			segment_writer,
			encryption_params.key,
			encryption_params.iv,
			&segment_writers);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_init_fmp4_frame_processor: mp4_cbcs_encrypt_get_writers failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
		per_stream_writer = TRUE;
	}
	else
#endif // NGX_HAVE_OPENSSL_EVP
	{
		segment_writers = segment_writer;
		per_stream_writer = FALSE;
	}

	if (submodule_context->media_set.total_track_count > 1)
	{
#if (NGX_HAVE_OPENSSL_EVP)
		if (encryption_params.type == HLS_ENC_SAMPLE_AES_CENC)
		{
			ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_init_fmp4_frame_processor: multiple streams not supported for sample aes cenc");
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
		}
#endif // NGX_HAVE_OPENSSL_EVP

		// muxed segment
		rc = mp4_muxer_init_fragment(
			&submodule_context->request_context,
			submodule_context->request_params.segment_index,
			&submodule_context->media_set,
			segment_writers,
			per_stream_writer,
			reuse_input_buffers,
			size_only,
			output_buffer,
			response_size,
			&muxer_state);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_init_fmp4_frame_processor: mp4_muxer_init_fragment failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}

		*frame_processor = (ngx_http_vod_frame_processor_t)mp4_muxer_process_frames;
		*frame_processor_state = muxer_state;
	}
	else
	{
#if (NGX_HAVE_OPENSSL_EVP)
		if (encryption_params.type == HLS_ENC_SAMPLE_AES_CENC)
		{
			drm_writer = *segment_writer;		// must not change segment_writer, otherwise the header will be encrypted

			// encyrpted fragment
			rc = edash_packager_get_fragment_writer(
				&drm_writer,
				&submodule_context->request_context,
				&submodule_context->media_set,
				submodule_context->request_params.segment_index,
				conf->min_single_nalu_per_frame_segment > 0 &&
				submodule_context->media_set.initial_segment_clip_relative_index >= conf->min_single_nalu_per_frame_segment - 1,
				submodule_context->media_set.sequences[0].encryption_key,		// iv
				size_only,
				output_buffer,
				response_size);
			switch (rc)
			{
			case VOD_DONE:		// passthrough
				break;

			case VOD_OK:
				segment_writers = &drm_writer;
				break;

			default:
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
					"ngx_http_vod_hls_init_fmp4_frame_processor: edash_packager_get_fragment_writer failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
			}
		}
		else
#endif // NGX_HAVE_OPENSSL_EVP
		{
			// for single stream use dash segments
			ngx_memzero(&header_extensions, sizeof(header_extensions));

			rc = dash_packager_build_fragment_header(
				&submodule_context->request_context,
				&submodule_context->media_set,
				submodule_context->request_params.segment_index,
				0,	// sample description index
				&header_extensions,
				size_only,
				output_buffer,
				response_size);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
					"ngx_http_vod_hls_init_fmp4_frame_processor: dash_packager_build_fragment_header failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
			}
		}

		// initialize the frame processor
		if (!size_only || *response_size == 0)
		{
			rc = mp4_fragment_frame_writer_init(
				&submodule_context->request_context,
				submodule_context->media_set.sequences,
				segment_writers[0].write_tail,
				segment_writers[0].context,
				reuse_input_buffers,
				&state);
			if (rc != VOD_OK)
			{
				ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
					"ngx_http_vod_hls_init_fmp4_frame_processor: mp4_fragment_frame_writer_init failed %i", rc);
				return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
			}

			*frame_processor = (ngx_http_vod_frame_processor_t)mp4_fragment_frame_writer_process;
			*frame_processor_state = state;
		}
	}

#if (NGX_HAVE_OPENSSL_EVP)
	if (encryption_params.type == HLS_ENC_AES_128)
	{
		*response_size = aes_round_up_to_block(*response_size);
	}
#endif // NGX_HAVE_OPENSSL_EVP

	// set the 'Content-type' header
	mp4_fragment_get_content_type(
		submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO],
		content_type);
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
		FALSE,
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
	PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE | PARSE_FLAG_KEY_FRAME_BITRATE | PARSE_FLAG_CODEC_NAME | PARSE_FLAG_PARSED_EXTRA_DATA_SIZE | PARSE_FLAG_CODEC_TRANSFER_CHAR,
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
	SUPPORTED_CODECS_TS,
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
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA | PARSE_FLAG_INITIAL_PTS_DELAY,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_TS,
	HLS_TIMESCALE,
	NULL,
	ngx_http_vod_hls_init_ts_frame_processor,
};

static const ngx_http_vod_request_t hls_mp4_segment_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_INITIAL_PTS_DELAY,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	HLS_TIMESCALE,
	NULL,
	ngx_http_vod_hls_init_fmp4_frame_processor,
};

static const ngx_http_vod_request_t hls_mp4_segment_request_cbcs = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_INITIAL_PTS_DELAY,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	HLS_TIMESCALE,
	NULL,
	ngx_http_vod_hls_init_fmp4_frame_processor,
};

static const ngx_http_vod_request_t hls_mp4_segment_request_cenc = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA | PARSE_FLAG_INITIAL_PTS_DELAY,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	HLS_TIMESCALE,
	NULL,
	ngx_http_vod_hls_init_fmp4_frame_processor,
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

static const ngx_http_vod_request_t hls_mp4_init_request = {
	REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE,
	PARSE_BASIC_METADATA_ONLY | PARSE_FLAG_SAVE_RAW_ATOMS,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS_MP4,
	HLS_TIMESCALE,
	ngx_http_vod_hls_handle_mp4_init_segment,
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
	conf->interleave_frames = NGX_CONF_UNSET;
	conf->align_frames = NGX_CONF_UNSET;
	conf->align_pts = NGX_CONF_UNSET;
	conf->output_id3_timestamps = NGX_CONF_UNSET;
	conf->encryption_method = NGX_CONF_UNSET_UINT;
	conf->output_iv = NGX_CONF_UNSET;
	conf->m3u8_config.output_iframes_playlist = NGX_CONF_UNSET;
	conf->m3u8_config.force_unmuxed_segments = NGX_CONF_UNSET;
	conf->m3u8_config.container_format = NGX_CONF_UNSET_UINT;
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
	ngx_conf_merge_value(conf->output_iv, prev->output_iv, 0);
	ngx_conf_merge_value(conf->m3u8_config.output_iframes_playlist, prev->m3u8_config.output_iframes_playlist, 1);

	ngx_conf_merge_str_value(conf->master_file_name_prefix, prev->master_file_name_prefix, "master");
	ngx_conf_merge_str_value(conf->m3u8_config.index_file_name_prefix, prev->m3u8_config.index_file_name_prefix, "index");
	ngx_conf_merge_str_value(conf->m3u8_config.iframes_file_name_prefix, prev->m3u8_config.iframes_file_name_prefix, "iframes");
	ngx_conf_merge_str_value(conf->m3u8_config.segment_file_name_prefix, prev->m3u8_config.segment_file_name_prefix, "seg");
	ngx_conf_merge_str_value(conf->m3u8_config.init_file_name_prefix, prev->m3u8_config.init_file_name_prefix, "init");

	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_file_name, prev->m3u8_config.encryption_key_file_name, "encryption");
	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_format, prev->m3u8_config.encryption_key_format, "");
	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_format_versions, prev->m3u8_config.encryption_key_format_versions, "");
	if (conf->encryption_key_uri == NULL)
	{
		conf->encryption_key_uri = prev->encryption_key_uri;
	}
	ngx_conf_merge_value(conf->m3u8_config.force_unmuxed_segments, prev->m3u8_config.force_unmuxed_segments, 0);
	ngx_conf_merge_uint_value(conf->m3u8_config.container_format, prev->m3u8_config.container_format, HLS_CONTAINER_AUTO);

	ngx_conf_merge_value(conf->interleave_frames, prev->interleave_frames, 0);
	ngx_conf_merge_value(conf->align_frames, prev->align_frames, 1);
	ngx_conf_merge_value(conf->align_pts, prev->align_pts, 0);
	ngx_conf_merge_value(conf->output_id3_timestamps, prev->output_id3_timestamps, 0);
	if (conf->id3_data == NULL)
	{
		conf->id3_data = prev->id3_data;
	}

	ngx_conf_merge_uint_value(conf->encryption_method, prev->encryption_method, HLS_ENC_NONE);

	m3u8_builder_init_config(
		&conf->m3u8_config,
		base->segmenter.max_segment_duration, 
		conf->encryption_method);

	switch (conf->encryption_method)
	{
	case HLS_ENC_NONE:
		break;

	case HLS_ENC_SAMPLE_AES_CENC:
		if (!base->drm_enabled)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"drm must be enabled when \"vod_hls_encryption_method\" is sample-aes-cenc");
			return NGX_CONF_ERROR;
		}
		break;

	default:
		if (base->secret_key == NULL &&
			!base->drm_enabled)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_secret_key\" must be set when \"vod_hls_encryption_method\" is not none");
			return NGX_CONF_ERROR;
		}
		break;
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
	// fmp4 segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.segment_file_name_prefix, m4s_file_ext))
	{
		start_pos += conf->hls.m3u8_config.segment_file_name_prefix.len;
		end_pos -= (sizeof(m4s_file_ext) - 1);

		switch (conf->hls.encryption_method)
		{
		case HLS_ENC_SAMPLE_AES:
			*request = &hls_mp4_segment_request_cbcs;
			break;

		case HLS_ENC_SAMPLE_AES_CENC:
			*request = &hls_mp4_segment_request_cenc;
			break;

		default:
			*request = &hls_mp4_segment_request;
			break;
		}

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
		!conf->drm_enabled &&
		conf->hls.encryption_method != HLS_ENC_NONE)
	{
		start_pos += conf->hls.m3u8_config.encryption_key_file_name.len;
		end_pos -= (sizeof(key_file_ext) - 1);
		*request = &hls_enc_key_request;
		flags = 0;
	}
	// init segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.init_file_name_prefix, mp4_file_ext))
	{
		start_pos += conf->hls.m3u8_config.init_file_name_prefix.len;
		end_pos -= (sizeof(mp4_file_ext) - 1);
		*request = &hls_mp4_init_request;
		flags = PARSE_FILE_NAME_ALLOW_CLIP_INDEX;
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
