#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/mp4/mp4_fragment.h"
#include "vod/mss/mss_packager.h"
#include "vod/subtitle/ttml_builder.h"
#include "vod/udrm.h"

#if (NGX_HAVE_OPENSSL_EVP)
#include "vod/mss/mss_playready.h"
#endif // NGX_HAVE_OPENSSL_EVP

// constants
#define SUPPORTED_CODECS (VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(AAC) | VOD_CODEC_FLAG(MP3))

// typedefs
typedef struct {
	uint64_t bitrate;
	uint64_t time;
	ngx_str_t media_type;
} fragment_params_t;

// constants
static const ngx_http_vod_match_definition_t fragment_match_definition[] = {
	{ MATCH_FIXED_STRING,	0,											0,	ngx_string("QualityLevels(") },
	{ MATCH_NUMBER,			offsetof(fragment_params_t, bitrate),		0,	ngx_null_string },
	{ MATCH_FIXED_STRING,	0,											0,	ngx_string(")/Fragments(") },
	{ MATCH_DELIM_STRING,	offsetof(fragment_params_t, media_type),	'=',ngx_null_string },
	{ MATCH_NUMBER,			offsetof(fragment_params_t, time),			0,	ngx_null_string },
	{ MATCH_FIXED_STRING,	0,											0,	ngx_string(")") },
	{ MATCH_END,			0,											0,	ngx_null_string },
};

// content types
static u_char manifest_content_type[] = "text/xml";		// TODO: consider moving to application/vnd.ms-sstr+xml

// extensions
static const u_char m4s_file_ext[] = ".m4s";

static ngx_int_t 
ngx_http_vod_mss_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

#if (NGX_HAVE_OPENSSL_EVP)
	if (submodule_context->conf->drm_enabled)
	{
		rc = mss_playready_build_manifest(
			&submodule_context->request_context,
			&submodule_context->conf->mss.manifest_conf,
			&submodule_context->media_set,
			response);
	}
	else
#endif // NGX_HAVE_OPENSSL_EVP
	{
		rc = mss_packager_build_manifest(
			&submodule_context->request_context,
			&submodule_context->conf->mss.manifest_conf,
			&submodule_context->media_set,
			0,
			NULL,
			NULL,
			response);
	}
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_mss_handle_manifest: mss_packager_build_manifest failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	content_type->data = manifest_content_type;
	content_type->len = sizeof(manifest_content_type) - 1;

	return NGX_OK;
}


static ngx_int_t
ngx_http_vod_mss_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	fragment_writer_state_t* state;
	vod_status_t rc;
	bool_t reuse_buffers = FALSE;
	bool_t size_only = ngx_http_vod_submodule_size_only(submodule_context);

#if (NGX_HAVE_OPENSSL_EVP)
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	segment_writer_t drm_writer;

	if (conf->drm_enabled)
	{
		drm_writer = *segment_writer;		// must not change segment_writer, otherwise the header will be encrypted

		rc = mss_playready_get_fragment_writer(
			&drm_writer,
			&submodule_context->request_context,
			&submodule_context->media_set,
			submodule_context->request_params.segment_index,
			conf->min_single_nalu_per_frame_segment > 0 && submodule_context->request_params.segment_index >= conf->min_single_nalu_per_frame_segment - 1,
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
				"ngx_http_vod_mss_init_frame_processor: mss_playready_get_fragment_writer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
	}
	else
#endif // NGX_HAVE_OPENSSL_EVP
	{
		rc = mss_packager_build_fragment_header(
			&submodule_context->request_context,
			&submodule_context->media_set,
			submodule_context->request_params.segment_index,
			0,
			NULL,
			NULL,
			size_only,
			output_buffer,
			response_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_mss_init_frame_processor: mss_packager_build_fragment_header failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
		}
	}

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
				"ngx_http_vod_mss_init_frame_processor: mp4_fragment_frame_writer_init failed %i", rc);
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

static ngx_int_t
ngx_http_vod_mss_handle_ttml_fragment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	rc = ttml_build_mp4(
		&submodule_context->request_context,
		&submodule_context->media_set,
		submodule_context->request_params.segment_index,
		MSS_TIMESCALE,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_mss_handle_ttml_fragment: ttml_build_mp4 failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	mp4_fragment_get_content_type(TRUE, content_type);
	return NGX_OK;
}

static const ngx_http_vod_request_t mss_manifest_request = {
	REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE | REQUEST_FLAG_LOOK_AHEAD_SEGMENTS | REQUEST_FLAG_NO_DISCONTINUITY,
	PARSE_FLAG_TOTAL_SIZE_ESTIMATE | PARSE_FLAG_PARSED_EXTRA_DATA,
	REQUEST_CLASS_MANIFEST,
	SUPPORTED_CODECS | VOD_CODEC_FLAG(WEBVTT),
	MSS_TIMESCALE,
	ngx_http_vod_mss_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t mss_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK | REQUEST_FLAG_LOOK_AHEAD_SEGMENTS | REQUEST_FLAG_NO_DISCONTINUITY,
	PARSE_FLAG_FRAMES_ALL,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS,
	MSS_TIMESCALE,
	NULL,
	ngx_http_vod_mss_init_frame_processor,
};

static const ngx_http_vod_request_t mss_playready_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK | REQUEST_FLAG_LOOK_AHEAD_SEGMENTS | REQUEST_FLAG_NO_DISCONTINUITY,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS,
	MSS_TIMESCALE,
	NULL,
	ngx_http_vod_mss_init_frame_processor,
};

static const ngx_http_vod_request_t mss_ttml_request = {
	REQUEST_FLAG_SINGLE_TRACK | REQUEST_FLAG_NO_DISCONTINUITY,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_RELATIVE_TIMESTAMPS,
	REQUEST_CLASS_SEGMENT,
	VOD_CODEC_FLAG(WEBVTT),
	TTML_TIMESCALE,
	ngx_http_vod_mss_handle_ttml_fragment,
	NULL,
};

static void
ngx_http_vod_mss_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_mss_loc_conf_t *conf)
{
	conf->manifest_conf.duplicate_bitrate_threshold = NGX_CONF_UNSET_UINT;
}

static char *
ngx_http_vod_mss_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_mss_loc_conf_t *conf,
	ngx_http_vod_mss_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");
	ngx_conf_merge_uint_value(conf->manifest_conf.duplicate_bitrate_threshold, prev->manifest_conf.duplicate_bitrate_threshold, 4096);
	return NGX_CONF_OK;
}

static int 
ngx_http_vod_mss_get_file_path_components(ngx_str_t* uri)
{
	u_char* end_pos = uri->data + uri->len;

	if (end_pos[-1] == ')')
	{
		// parse "QualityLevels(2364883)/Fragments(video=0)" as the file part of the path
		return 2;
	}
	else
	{
		return 1;
	}
}

static ngx_int_t
ngx_http_vod_mss_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	fragment_params_t fragment_params;
	ngx_int_t rc;

	// fragment
	if (end_pos[-1] == ')')
	{
		if (!ngx_http_vod_parse_string(fragment_match_definition, start_pos, end_pos, &fragment_params))
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_mss_parse_uri_file_name: ngx_http_vod_parse_string failed");
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		request_params->sequences_mask = (1 << mss_sequence_index(fragment_params.bitrate));

		request_params->segment_time = fragment_params.time / 10000;

		// Note: assuming no discontinuity, if this changes the segment index will be recalculated
		request_params->segment_index = segmenter_get_segment_index_no_discontinuity(
			&conf->segmenter,
			request_params->segment_time + SEGMENT_FROM_TIMESTAMP_MARGIN);

		if (fragment_params.media_type.len == sizeof(MSS_STREAM_TYPE_VIDEO) - 1 &&
			ngx_memcmp(fragment_params.media_type.data, MSS_STREAM_TYPE_VIDEO, sizeof(MSS_STREAM_TYPE_VIDEO) - 1) == 0)
		{
			vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_VIDEO]);
			vod_set_bit(request_params->tracks_mask[MEDIA_TYPE_VIDEO], mss_track_index(fragment_params.bitrate));
		}
		else if (fragment_params.media_type.len == sizeof(MSS_STREAM_TYPE_AUDIO) - 1 &&
			ngx_memcmp(fragment_params.media_type.data, MSS_STREAM_TYPE_AUDIO, sizeof(MSS_STREAM_TYPE_AUDIO) - 1) == 0)
		{
			vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_AUDIO]);
			vod_set_bit(request_params->tracks_mask[MEDIA_TYPE_AUDIO], mss_track_index(fragment_params.bitrate));
		}
		else if (fragment_params.media_type.len == sizeof(MSS_STREAM_TYPE_TEXT) - 1 &&
			ngx_memcmp(fragment_params.media_type.data, MSS_STREAM_TYPE_TEXT, sizeof(MSS_STREAM_TYPE_TEXT) - 1) == 0)
		{
			vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_SUBTITLE]);
			vod_set_bit(request_params->tracks_mask[MEDIA_TYPE_SUBTITLE], 0);
			*request = &mss_ttml_request;
			return NGX_OK;
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_mss_parse_uri_file_name: invalid media type %V", &fragment_params.media_type);
			return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
		}

		*request = conf->drm_enabled ? &mss_playready_fragment_request : &mss_fragment_request;

		return NGX_OK;
	}
	// manifest
	else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->mss.manifest_file_name_prefix))
	{
		*request = &mss_manifest_request;
		start_pos += conf->mss.manifest_file_name_prefix.len;

		rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE, request_params);
		if (rc != NGX_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"ngx_http_vod_mss_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
			return rc;
		}

		return NGX_OK;
	}
	// fragment with non-standard format (used with redirect)
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.segment_file_name_prefix, m4s_file_ext))
	{
		start_pos += conf->hls.m3u8_config.segment_file_name_prefix.len;
		end_pos -= (sizeof(m4s_file_ext) - 1);
		*request = conf->drm_enabled ? &mss_playready_fragment_request : &mss_fragment_request;

		return ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, PARSE_FILE_NAME_EXPECT_SEGMENT_INDEX, request_params);
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_mss_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}
}

static ngx_int_t
ngx_http_vod_mss_parse_drm_info(
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

DEFINE_SUBMODULE(mss);
