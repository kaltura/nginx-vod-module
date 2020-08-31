#include <ngx_http.h>
#include <ngx_md5.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/dash/dash_packager.h"
#include "vod/mkv/mkv_builder.h"
#include "vod/mp4/mp4_fragment.h"
#include "vod/mp4/mp4_init_segment.h"
#include "vod/subtitle/webvtt_builder.h"
#include "vod/subtitle/ttml_builder.h"
#include "vod/udrm.h"

#if (NGX_HAVE_OPENSSL_EVP)
#include "vod/dash/edash_packager.h"
#endif // NGX_HAVE_OPENSSL_EVP

// constants
#define SUPPORTED_CODECS_MP4 (VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(HEVC) | VOD_CODEC_FLAG(AAC) | VOD_CODEC_FLAG(AC3) | VOD_CODEC_FLAG(EAC3))
#define SUPPORTED_CODECS_WEBM (VOD_CODEC_FLAG(VP8) | VOD_CODEC_FLAG(VP9) | VOD_CODEC_FLAG(AV1) | VOD_CODEC_FLAG(VORBIS) | VOD_CODEC_FLAG(OPUS))
#define SUPPORTED_CODECS (SUPPORTED_CODECS_MP4 | SUPPORTED_CODECS_WEBM)

ngx_conf_enum_t  dash_manifest_formats[] = {
	{ ngx_string("segmentlist"), FORMAT_SEGMENT_LIST },
	{ ngx_string("segmenttemplate"), FORMAT_SEGMENT_TEMPLATE },
	{ ngx_string("segmenttimeline"), FORMAT_SEGMENT_TIMELINE },
	{ ngx_null_string, 0 }
};

ngx_conf_enum_t  dash_subtitle_formats[] = {
	{ ngx_string("webvtt"), SUBTITLE_FORMAT_WEBVTT },
	{ ngx_string("smpte-tt"), SUBTITLE_FORMAT_SMPTE_TT },
	{ ngx_null_string, 0 }
};

// content types
static u_char mpd_content_type[] = "application/dash+xml";
static u_char webm_audio_content_type[] = "audio/webm";
static u_char webm_video_content_type[] = "video/webm";
static u_char vtt_content_type[] = "text/vtt";

// file extensions
static const u_char manifest_file_ext[] = ".mpd";
static const u_char init_segment_file_ext[] = ".mp4";
static const u_char fragment_file_ext[] = ".m4s";
static const u_char webm_file_ext[] = ".webm";
static const u_char vtt_file_ext[] = ".vtt";
static const u_char ttml_file_ext[] = ".ttml";

static ngx_int_t 
ngx_http_vod_dash_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	dash_manifest_extensions_t extensions;
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	ngx_str_t file_uri;

	if (conf->dash.absolute_manifest_urls)
	{
		if (conf->dash.mpd_config.manifest_format == FORMAT_SEGMENT_LIST)
		{
			file_uri.data = NULL;
			file_uri.len = 0;
		}
		else
		{
			file_uri = submodule_context->r->uri;
		}

		rc = ngx_http_vod_get_base_url(submodule_context->r, conf->base_url, &file_uri, &base_url);
		if (rc != NGX_OK)
		{
			return rc;
		}
	}

#if (NGX_HAVE_OPENSSL_EVP)
	if (conf->drm_enabled)
	{
		rc = edash_packager_build_mpd(
			&submodule_context->request_context,
			&conf->dash.mpd_config,
			&base_url,
			&submodule_context->media_set,
			conf->drm_single_key,
			response);
	}
	else
#endif // NGX_HAVE_OPENSSL_EVP
	{
		vod_memzero(&extensions, sizeof(extensions));

		rc = dash_packager_build_mpd(
			&submodule_context->request_context,
			&conf->dash.mpd_config,
			&base_url,
			&submodule_context->media_set,
			&extensions,
			response);
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_handle_manifest: (e)dash_packager_build_mpd failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	content_type->data = mpd_content_type;
	content_type->len = sizeof(mpd_content_type) - 1;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_mp4_handle_init_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

#if (NGX_HAVE_OPENSSL_EVP)
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	uint32_t flags;

	if (conf->drm_enabled)
	{
		flags = 0;

		if (conf->dash.init_mp4_pssh)
		{
			flags |= EDASH_INIT_MP4_WRITE_PSSH;
		}

		if (conf->drm_clear_lead_segment_count > 0)
		{
			flags |= EDASH_INIT_MP4_HAS_CLEAR_LEAD;
		}

		rc = edash_packager_build_init_mp4(
			&submodule_context->request_context,
			&submodule_context->media_set,
			flags,
			ngx_http_vod_submodule_size_only(submodule_context),
			response);
	}
	else
#endif // NGX_HAVE_OPENSSL_EVP
	{
		rc = mp4_init_segment_build(
			&submodule_context->request_context,
			&submodule_context->media_set,
			ngx_http_vod_submodule_size_only(submodule_context),
			NULL,
			NULL,
			response);
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_mp4_handle_init_segment: (e)dash_packager_build_init_mp4 failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	mp4_fragment_get_content_type(
		submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO],
		content_type);
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_mp4_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	dash_fragment_header_extensions_t header_extensions;
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	fragment_writer_state_t* state;
	vod_status_t rc;
	bool_t reuse_buffers = FALSE;
	bool_t size_only = ngx_http_vod_submodule_size_only(submodule_context);

#if (NGX_HAVE_OPENSSL_EVP)
	segment_writer_t drm_writer;

	if (conf->drm_enabled &&
		submodule_context->request_params.segment_index >= conf->drm_clear_lead_segment_count)
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
			segment_writer = &drm_writer;
			reuse_buffers = TRUE;		// mp4_cenc_encrypt allocates new buffers
			break;

		default:
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_mp4_init_frame_processor: edash_packager_get_fragment_writer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
	}
	else
#endif // NGX_HAVE_OPENSSL_EVP
	{
		// unencrypted
		ngx_memzero(&header_extensions, sizeof(header_extensions));

		rc = dash_packager_build_fragment_header(
			&submodule_context->request_context,
			&submodule_context->media_set,
			submodule_context->request_params.segment_index,
			conf->drm_enabled ? 2 : 0,	// sample description index
			&header_extensions,
			size_only,
			output_buffer,
			response_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_mp4_init_frame_processor: dash_packager_build_fragment_header failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
	}

	// initialize the frame processor
	if (!size_only || *response_size == 0)
	{
		rc = mp4_fragment_frame_writer_init(
			&submodule_context->request_context,
			submodule_context->media_set.sequences,
			segment_writer->write_tail,
			segment_writer->context,
			reuse_buffers,
			&state);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_mp4_init_frame_processor: mp4_fragment_frame_writer_init failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}

		*frame_processor = (ngx_http_vod_frame_processor_t)mp4_fragment_frame_writer_process;
		*frame_processor_state = state;
	}

	// set the 'Content-type' header
	mp4_fragment_get_content_type(
		submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO],
		content_type);
	return NGX_OK;
}

static void
ngx_http_vod_dash_get_webm_content_type(
	bool_t video,
	ngx_str_t* content_type)
{
	if (video)
	{
		content_type->data = webm_video_content_type;
		content_type->len = sizeof(webm_video_content_type) - 1;
	}
	else
	{
		content_type->data = webm_audio_content_type;
		content_type->len = sizeof(webm_audio_content_type) - 1;
	}
}

static ngx_int_t
ngx_http_vod_dash_webm_handle_init_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;
	ngx_md5_t md5;
	ngx_str_t* uri;
	uint64_t track_uid;
	u_char uri_key[16];

	// calculate a uid for track based on the request uri
	uri = &submodule_context->r->uri;
	ngx_md5_init(&md5);
	ngx_md5_update(&md5, uri->data, uri->len);
	ngx_md5_final(uri_key, &md5);
	ngx_memcpy(&track_uid, uri_key, sizeof(track_uid));

	rc = mkv_build_init_segment(
		&submodule_context->request_context,
		submodule_context->media_set.sequences[0].filtered_clips[0].first_track,
		track_uid,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_webm_handle_init_segment: mkv_build_init_segment failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	ngx_http_vod_dash_get_webm_content_type(
		submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO],
		content_type);
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_webm_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	mkv_encryption_type_t encryption_type;
	vod_status_t rc;

	if (conf->drm_enabled)
	{
		if (submodule_context->request_params.segment_index >= conf->drm_clear_lead_segment_count)
		{
			encryption_type = MKV_ENCRYPTED;
		}
		else
		{
			encryption_type = MKV_CLEAR_LEAD;
		}
	}
	else
	{
		encryption_type = MKV_CLEAR;
	}

	rc = mkv_builder_frame_writer_init(
		&submodule_context->request_context,
		submodule_context->media_set.sequences,
		segment_writer->write_tail,
		segment_writer->context,
		FALSE,
		encryption_type,
		submodule_context->media_set.sequences[0].encryption_key,
		output_buffer,
		response_size,
		frame_processor_state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_webm_init_frame_processor: mkv_builder_frame_writer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)mkv_builder_frame_writer_process;

	// set the 'Content-type' header
	ngx_http_vod_dash_get_webm_content_type(
		submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO],
		content_type);
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_handle_vtt_file(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	rc = webvtt_builder_build(
		&submodule_context->request_context,
		&submodule_context->media_set,
		submodule_context->media_set.use_discontinuity,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_handle_vtt_file: webvtt_builder_build failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	content_type->len = sizeof(vtt_content_type) - 1;
	content_type->data = (u_char *)vtt_content_type;
	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_handle_ttml_fragment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	dash_fragment_header_extensions_t header_extensions;
	vod_status_t rc;
	size_t ignore;

	ngx_memzero(&header_extensions, sizeof(header_extensions));

	header_extensions.mdat_atom_max_size = ttml_builder_get_max_size(&submodule_context->media_set);
	header_extensions.write_mdat_atom_callback = (dash_write_mdat_atom_callback_t)ttml_builder_write;
	header_extensions.write_mdat_atom_context = &submodule_context->media_set;

	rc = dash_packager_build_fragment_header(
		&submodule_context->request_context,
		&submodule_context->media_set,
		submodule_context->request_params.segment_index,
		0,
		&header_extensions,
		FALSE,
		response,
		&ignore);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_handle_ttml_fragment: dash_packager_build_fragment_header failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	mp4_fragment_get_content_type(TRUE, content_type);
	return NGX_OK;
}

static const ngx_http_vod_request_t dash_manifest_request = {
	REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE,
	PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE | PARSE_FLAG_INITIAL_PTS_DELAY | PARSE_FLAG_CODEC_NAME,
	REQUEST_CLASS_MANIFEST,
	SUPPORTED_CODECS | VOD_CODEC_FLAG(WEBVTT),
	DASH_TIMESCALE,
	ngx_http_vod_dash_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t dash_mp4_init_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_BASIC_METADATA_ONLY | PARSE_FLAG_SAVE_RAW_ATOMS,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS_MP4 | VOD_CODEC_FLAG(WEBVTT),
	DASH_TIMESCALE,
	ngx_http_vod_dash_mp4_handle_init_segment,
	NULL,
};

static const ngx_http_vod_request_t dash_mp4_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_INITIAL_PTS_DELAY,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	DASH_TIMESCALE,
	NULL,
	ngx_http_vod_dash_mp4_init_frame_processor,
};

static const ngx_http_vod_request_t edash_mp4_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_INITIAL_PTS_DELAY | PARSE_FLAG_PARSED_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	DASH_TIMESCALE,
	NULL,
	ngx_http_vod_dash_mp4_init_frame_processor,
};

static const ngx_http_vod_request_t dash_webm_init_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_BASIC_METADATA_ONLY,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS_WEBM,
	DASH_TIMESCALE,
	ngx_http_vod_dash_webm_handle_init_segment,
	NULL,
};

static const ngx_http_vod_request_t dash_webm_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_INITIAL_PTS_DELAY,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_WEBM,
	DASH_TIMESCALE,
	NULL,
	ngx_http_vod_dash_webm_init_frame_processor,
};

static const ngx_http_vod_request_t dash_webvtt_file_request = {
	REQUEST_FLAG_SINGLE_TRACK | REQUEST_FLAG_PARSE_ALL_CLIPS,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_OTHER,
	VOD_CODEC_FLAG(WEBVTT),
	WEBVTT_TIMESCALE,
	ngx_http_vod_dash_handle_vtt_file,
	NULL,
};

static const ngx_http_vod_request_t dash_ttml_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	VOD_CODEC_FLAG(WEBVTT),
	TTML_TIMESCALE,
	ngx_http_vod_dash_handle_ttml_fragment,
	NULL,
};

static void
ngx_http_vod_dash_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_dash_loc_conf_t *conf)
{
	conf->absolute_manifest_urls = NGX_CONF_UNSET;
	conf->init_mp4_pssh = NGX_CONF_UNSET;
	conf->mpd_config.manifest_format = NGX_CONF_UNSET_UINT;
	conf->mpd_config.subtitle_format = NGX_CONF_UNSET_UINT;
	conf->mpd_config.duplicate_bitrate_threshold = NGX_CONF_UNSET_UINT;
	conf->mpd_config.write_playready_kid = NGX_CONF_UNSET;
	conf->mpd_config.use_base_url_tag = NGX_CONF_UNSET;
}

static char *
ngx_http_vod_dash_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_dash_loc_conf_t *conf,
	ngx_http_vod_dash_loc_conf_t *prev)
{
	ngx_conf_merge_value(conf->absolute_manifest_urls, prev->absolute_manifest_urls, 1);
	ngx_conf_merge_value(conf->init_mp4_pssh, prev->init_mp4_pssh, 1);

	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");
	ngx_conf_merge_str_value(conf->mpd_config.profiles, prev->mpd_config.profiles, "urn:mpeg:dash:profile:isoff-main:2011");
	ngx_conf_merge_str_value(conf->mpd_config.init_file_name_prefix, prev->mpd_config.init_file_name_prefix, "init");
	ngx_conf_merge_str_value(conf->mpd_config.fragment_file_name_prefix, prev->mpd_config.fragment_file_name_prefix, "fragment");
	ngx_conf_merge_str_value(conf->mpd_config.subtitle_file_name_prefix, prev->mpd_config.subtitle_file_name_prefix, "sub");
	ngx_conf_merge_uint_value(conf->mpd_config.manifest_format, prev->mpd_config.manifest_format, FORMAT_SEGMENT_TIMELINE);
	ngx_conf_merge_uint_value(conf->mpd_config.subtitle_format, prev->mpd_config.subtitle_format, SUBTITLE_FORMAT_WEBVTT);
	ngx_conf_merge_uint_value(conf->mpd_config.duplicate_bitrate_threshold, prev->mpd_config.duplicate_bitrate_threshold, 4096);
	ngx_conf_merge_value(conf->mpd_config.write_playready_kid, prev->mpd_config.write_playready_kid, 0);
	ngx_conf_merge_value(conf->mpd_config.use_base_url_tag, prev->mpd_config.use_base_url_tag, 0);

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
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	ngx_int_t rc;
	uint32_t flags;

	// fragment
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.fragment_file_name_prefix, fragment_file_ext))
	{
		start_pos += conf->dash.mpd_config.fragment_file_name_prefix.len;
		end_pos -= (sizeof(fragment_file_ext) - 1);
		*request = conf->drm_enabled ? &edash_mp4_fragment_request : &dash_mp4_fragment_request;
		flags = PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX;
	}
	// init segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.init_file_name_prefix, init_segment_file_ext))
	{
		start_pos += conf->dash.mpd_config.init_file_name_prefix.len;
		end_pos -= (sizeof(init_segment_file_ext) - 1);
		*request = &dash_mp4_init_request;
		flags = PARSE_FILE_NAME_ALLOW_CLIP_INDEX;
	}
	// webm fragment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.fragment_file_name_prefix, webm_file_ext))
	{
		start_pos += conf->dash.mpd_config.fragment_file_name_prefix.len;
		end_pos -= (sizeof(webm_file_ext) - 1);
		*request = &dash_webm_fragment_request;
		flags = PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX;
	}
	// webm init segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.init_file_name_prefix, webm_file_ext))
	{
		start_pos += conf->dash.mpd_config.init_file_name_prefix.len;
		end_pos -= (sizeof(webm_file_ext) - 1);
		*request = &dash_webm_init_request;
		flags = PARSE_FILE_NAME_ALLOW_CLIP_INDEX;
	}
	// manifest
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.manifest_file_name_prefix, manifest_file_ext))
	{
		start_pos += conf->dash.manifest_file_name_prefix.len;
		end_pos -= (sizeof(manifest_file_ext) - 1);
		*request = &dash_manifest_request;
		flags = PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE;
	}
	// smpte fragment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.fragment_file_name_prefix, ttml_file_ext))
	{
		start_pos += conf->dash.mpd_config.fragment_file_name_prefix.len;
		end_pos -= (sizeof(ttml_file_ext) - 1);
		*request = &dash_ttml_request;
		flags = PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX;
	}
	// webvtt file
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.subtitle_file_name_prefix, vtt_file_ext))
	{
		start_pos += conf->dash.mpd_config.subtitle_file_name_prefix.len;
		end_pos -= (sizeof(vtt_file_ext) - 1);
		*request = &dash_webvtt_file_request;
		flags = PARSE_FILE_NAME_ALLOW_CLIP_INDEX;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_dash_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, flags, request_params);
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

DEFINE_SUBMODULE(dash);
