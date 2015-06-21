#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "ngx_http_vod_udrm.h"
#include "vod/mss/mss_packager.h"
#include "vod/mss/mss_playready.h"

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
static u_char mp4_audio_content_type[] = "audio/mp4";
static u_char mp4_video_content_type[] = "video/mp4";
static u_char manifest_content_type[] = "text/xml";

static ngx_int_t 
ngx_http_vod_mss_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	if (submodule_context->conf->drm_enabled)
	{
		rc = mss_playready_build_manifest(
			&submodule_context->request_context,
			&submodule_context->conf->segmenter,
			&submodule_context->mpeg_metadata,
			response);
	}
	else
	{
		rc = mss_packager_build_manifest(
			&submodule_context->request_context,
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
			"ngx_http_vod_mss_handle_manifest: mss_packager_build_manifest failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = manifest_content_type;
	content_type->len = sizeof(manifest_content_type) - 1;

	return NGX_OK;
}


static ngx_int_t
ngx_http_vod_mss_init_frame_processor(
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
	segment_writer_t drm_writer;
	vod_status_t rc;
	bool_t size_only = ngx_http_vod_submodule_size_only(submodule_context);

	if (submodule_context->conf->drm_enabled)
	{
		rc = mss_playready_get_fragment_writer(
			&drm_writer,
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
				"ngx_http_vod_mss_init_frame_processor: mss_playready_get_fragment_writer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		segment_writer = &drm_writer;
	}
	else
	{
		rc = mss_packager_build_fragment_header(
			&submodule_context->request_context,
			submodule_context->mpeg_metadata.first_stream,
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
			return ngx_http_vod_status_to_ngx_error(rc);
		}
	}

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
				"ngx_http_vod_mss_init_frame_processor: mp4_builder_frame_writer_init failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		*frame_processor = (ngx_http_vod_frame_processor_t)mp4_builder_frame_writer_process;
		*frame_processor_state = state;
	}

	// set the 'Content-type' header
	if (submodule_context->mpeg_metadata.stream_count[MEDIA_TYPE_VIDEO])
	{
		content_type->len = sizeof(mp4_video_content_type) - 1;
		content_type->data = mp4_video_content_type;
	}
	else
	{
		content_type->len = sizeof(mp4_audio_content_type) - 1;
		content_type->data = mp4_audio_content_type;
	}

	return NGX_OK;
}

static const ngx_http_vod_request_t mss_manifest_request = {
	0,
	PARSE_FLAG_TOTAL_SIZE_ESTIMATE | PARSE_FLAG_PARSED_EXTRA_DATA,
	mss_packager_compare_streams,
	offsetof(ngx_http_vod_loc_conf_t, duplicate_bitrate_threshold),
	REQUEST_CLASS_MANIFEST,
	ngx_http_vod_mss_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t mss_fragment_request = {
	REQUEST_FLAG_SINGLE_STREAM,
	PARSE_FLAG_FRAMES_ALL,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_mss_init_frame_processor,
};

static const ngx_http_vod_request_t mss_playready_fragment_request = {
	REQUEST_FLAG_SINGLE_STREAM,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_mss_init_frame_processor,
};

void
ngx_http_vod_mss_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_mss_loc_conf_t *conf)
{
}

static char *
ngx_http_vod_mss_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_mss_loc_conf_t *conf,
	ngx_http_vod_mss_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");
	return NGX_CONF_OK;
}

int 
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

ngx_int_t
ngx_http_vod_mss_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	ngx_http_vod_request_params_t* request_params)
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
			return NGX_HTTP_BAD_REQUEST;
		}

		if (fragment_params.media_type.len != sizeof(MSS_STREAM_TYPE_VIDEO) - 1)
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_mss_parse_uri_file_name: invalid media type length %uz", fragment_params.media_type.len);
			return NGX_HTTP_BAD_REQUEST;
		}

		request_params->required_files = (1 << mss_file_index(fragment_params.bitrate));

		if (ngx_memcmp(fragment_params.media_type.data, MSS_STREAM_TYPE_VIDEO, sizeof(MSS_STREAM_TYPE_VIDEO) - 1) == 0)
		{
			request_params->required_tracks[MEDIA_TYPE_VIDEO] = (1 << mss_track_index(fragment_params.bitrate));
		}
		else if (ngx_memcmp(fragment_params.media_type.data, MSS_STREAM_TYPE_AUDIO, sizeof(MSS_STREAM_TYPE_AUDIO) - 1) == 0)
		{
			request_params->required_tracks[MEDIA_TYPE_AUDIO] = (1 << mss_track_index(fragment_params.bitrate));
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_mss_parse_uri_file_name: invalid media type %V", &fragment_params.media_type);
			return NGX_HTTP_BAD_REQUEST;
		}

		request_params->segment_index = segmenter_get_segment_index(&conf->segmenter, fragment_params.time / 10000);

		request_params->request = conf->drm_enabled ? &mss_playready_fragment_request : &mss_fragment_request;

		return NGX_OK;
	}

	// manifest
	if (!ngx_http_vod_starts_with(start_pos, end_pos, &conf->mss.manifest_file_name_prefix))
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_mss_parse_uri_file_name: unidentified request");
		return NGX_HTTP_BAD_REQUEST;
	}

	request_params->request = &mss_manifest_request;
	start_pos += conf->mss.manifest_file_name_prefix.len;

	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, FALSE, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_mss_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_mss_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	return 	ngx_http_vod_udrm_parse_response(
		&submodule_context->request_context,
		drm_info,
		output);
}

DEFINE_SUBMODULE(mss);
