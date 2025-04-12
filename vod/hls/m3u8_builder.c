#include "m3u8_builder.h"
#include "../manifest_utils.h"
#include "../mp4/mp4_defs.h"

#if (NGX_HAVE_OPENSSL_EVP)
#include "../dash/edash_packager.h"
#include "../mp4/mp4_defs.h"
#endif // NGX_HAVE_OPENSSL_EVP

// macros
#define M3U8_HEADER_PART1 "#EXTM3U\n#EXT-X-TARGETDURATION:%uL\n#EXT-X-ALLOW-CACHE:YES\n"
#define M3U8_HEADER_VOD "#EXT-X-PLAYLIST-TYPE:VOD\n"
#define M3U8_HEADER_EVENT "#EXT-X-PLAYLIST-TYPE:EVENT\n"
#define M3U8_HEADER_PART2 "#EXT-X-VERSION:%d\n#EXT-X-MEDIA-SEQUENCE:%uD\n"

#define M3U8_EXT_MEDIA_BASE "#EXT-X-MEDIA:TYPE=%s,GROUP-ID=\"%s%uD\",NAME=\"%V\","
#define M3U8_EXT_MEDIA_LANG "LANGUAGE=\"%V\","
#define M3U8_EXT_MEDIA_DEFAULT "DEFAULT=YES,"
#define M3U8_EXT_MEDIA_NON_DEFAULT "DEFAULT=NO,"
#define M3U8_EXT_MEDIA_AUTOSELECT "AUTOSELECT=YES,"
#define M3U8_EXT_MEDIA_FORCED "FORCED=YES,"
#define M3U8_EXT_MEDIA_CHARACTERISTICS "CHARACTERISTICS=\"%V\","
#define M3U8_EXT_MEDIA_URI "URI=\""
#define M3U8_EXT_MEDIA_INSTREAM_ID "INSTREAM-ID=\"%V\""

#define M3U8_EXT_MEDIA_CHANNELS "CHANNELS=\"%uD\","

#define M3U8_EXT_MEDIA_TYPE_AUDIO "AUDIO"
#define M3U8_EXT_MEDIA_TYPE_SUBTITLES "SUBTITLES"
#define M3U8_EXT_MEDIA_TYPE_CLOSED_CAPTIONS "CLOSED-CAPTIONS"

#define M3U8_EXT_MEDIA_GROUP_ID_AUDIO "audio"
#define M3U8_EXT_MEDIA_GROUP_ID_SUBTITLES "subs"
#define M3U8_EXT_MEDIA_GROUP_ID_CLOSED_CAPTIONS "cc"

#define M3U8_STREAM_TAG_AUDIO ",AUDIO=\"" M3U8_EXT_MEDIA_GROUP_ID_AUDIO "%uD\""
#define M3U8_STREAM_TAG_SUBTITLES ",SUBTITLES=\"" M3U8_EXT_MEDIA_GROUP_ID_SUBTITLES "%uD\""
#define M3U8_STREAM_TAG_CLOSED_CAPTIONS ",CLOSED-CAPTIONS=\"" M3U8_EXT_MEDIA_GROUP_ID_CLOSED_CAPTIONS "%uD\""
#define M3U8_STREAM_TAG_NO_CLOSED_CAPTIONS ",CLOSED-CAPTIONS=NONE"

#define M3U8_VIDEO_RANGE_SDR ",VIDEO-RANGE=SDR"
#define M3U8_VIDEO_RANGE_PQ ",VIDEO-RANGE=PQ"

#define MAX_CHARACTERISTICS_SIZE (128)

// constants
static const u_char m3u8_header[] = "#EXTM3U\n";
static const u_char m3u8_footer[] = "#EXT-X-ENDLIST\n";
static const char m3u8_stream_inf_video[] = "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%uD,RESOLUTION=%uDx%uD,FRAME-RATE=%uD.%03uD,CODECS=\"%V";
static const char m3u8_stream_inf_audio[] = "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%uD,CODECS=\"%V";
static const char m3u8_average_bandwidth[] = ",AVERAGE-BANDWIDTH=%uD";
static const char m3u8_iframe_stream_inf[] = "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=%uD,RESOLUTION=%uDx%uD,CODECS=\"%V\",URI=\"";
static const u_char m3u8_discontinuity[] = "#EXT-X-DISCONTINUITY\n";
static const char byte_range_tag_format[] = "#EXT-X-BYTERANGE:%uD@%uD\n";
static const u_char m3u8_url_suffix[] = ".m3u8";
static const u_char m3u8_map_prefix[] = "#EXT-X-MAP:URI=\"";
static const u_char m3u8_map_suffix[] = ".mp4\"\n";
static const char m3u8_clip_index[] = "-c%uD";


static const char encryption_key_tag_method[] = "#EXT-X-KEY:METHOD=";
static const char encryption_key_tag_uri[] = ",URI=\"";
static const char encryption_key_tag_iv[] = ",IV=0x";
static const char encryption_key_tag_key_format[] = ",KEYFORMAT=\"";
static const char encryption_key_tag_key_format_versions[] = ",KEYFORMATVERSIONS=\"";
static const char encryption_key_extension[] = ".key";
static const char encryption_type_aes_128[] = "AES-128";
static const char encryption_type_sample_aes[] = "SAMPLE-AES";
static const char encryption_type_sample_aes_cenc[] = "SAMPLE-AES-CTR";

#if (NGX_HAVE_OPENSSL_EVP)
static const char sample_aes_cenc_uri_prefix[] = "data:text/plain;base64,";
#endif // NGX_HAVE_OPENSSL_EVP

static vod_str_t m3u8_ts_suffix = vod_string(".ts\n");
static vod_str_t m3u8_m4s_suffix = vod_string(".m4s\n");
static vod_str_t m3u8_vtt_suffix = vod_string(".vtt\n");

static vod_str_t default_label = vod_string("default");

// typedefs
typedef struct {
	u_char* p;
	vod_str_t name_suffix;
	vod_str_t* base_url;
	vod_str_t* segment_file_name_prefix;
} write_segment_context_t;

// Notes: 
//	1. not using vod_sprintf in order to avoid the use of floats
//  2. scale must be a power of 10

static u_char* 
m3u8_builder_format_double(u_char* p, uint32_t n, uint32_t scale)
{
	int cur_digit;
	int int_n = n / scale;
	int fraction = n % scale;

	p = vod_sprintf(p, "%d", int_n);

	if (scale == 1)
	{
		return p;
	}

	*p++ = '.';
	for (;;)
	{
		scale /= 10;
		if (scale == 0)
		{
			break;
		}
		cur_digit = fraction / scale;
		*p++ = cur_digit + '0';
		fraction -= cur_digit * scale;
	}
	return p;
}

static u_char*
m3u8_builder_append_segment_name(
	u_char* p, 
	vod_str_t* base_url,
	vod_str_t* segment_file_name_prefix, 
	uint32_t segment_index, 
	vod_str_t* suffix)
{
	p = vod_copy(p, base_url->data, base_url->len);
	p = vod_copy(p, segment_file_name_prefix->data, segment_file_name_prefix->len);
	*p++ = '-';
	p = vod_sprintf(p, "%uD", segment_index + 1);
	p = vod_copy(p, suffix->data, suffix->len);
	return p;
}

static u_char*
m3u8_builder_append_extinf_tag(u_char* p, uint32_t duration, uint32_t scale)
{
	p = vod_copy(p, "#EXTINF:", sizeof("#EXTINF:") - 1);
	p = m3u8_builder_format_double(p, duration, scale);
	*p++ = ',';
	*p++ = '\n';
	return p;
}

static void
m3u8_builder_append_iframe_string(void* context, uint32_t segment_index, uint32_t frame_duration, uint32_t frame_start, uint32_t frame_size)
{
	write_segment_context_t* ctx = (write_segment_context_t*)context;

	ctx->p = m3u8_builder_append_extinf_tag(ctx->p, frame_duration, 1000);
	ctx->p = vod_sprintf(ctx->p, byte_range_tag_format, frame_size, frame_start);
	ctx->p = m3u8_builder_append_segment_name(
		ctx->p, 
		ctx->base_url,
		ctx->segment_file_name_prefix, 
		segment_index, 
		&ctx->name_suffix);
}

static vod_status_t
m3u8_builder_build_tracks_spec(
	request_context_t* request_context,
	media_set_t* media_set, 
	vod_str_t* suffix, 
	vod_str_t* result)
{
	media_track_t** cur_track_ptr;
	media_track_t** tracks_end;
	media_track_t** tracks;
	media_track_t* cur_track;
	u_char* p;
	size_t result_size;

	// get the result size
	result_size = suffix->len + 
		(sizeof("-v") - 1 + VOD_INT32_LEN) * media_set->total_track_count;
	if (media_set->has_multi_sequences)
	{
		result_size += (sizeof("-f") - 1 + VOD_INT32_LEN) * media_set->total_track_count;
	}

	// allocate the result buffer and the track ptrs array
	tracks = vod_alloc(request_context->pool, sizeof(tracks[0]) * media_set->total_track_count + result_size);
	if (tracks == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_tracks_spec: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// build the track ptrs array
	tracks_end = tracks + media_set->total_track_count;
	for (cur_track_ptr = tracks, cur_track = media_set->filtered_tracks; 
		cur_track_ptr < tracks_end; 
		cur_track_ptr++, cur_track++)
	{
		*cur_track_ptr = cur_track;
	}

	// write the result
	result->data = p = (u_char*)tracks_end;

	p = manifest_utils_append_tracks_spec(
		p,
		tracks,
		media_set->total_track_count,
		media_set->has_multi_sequences);

	p = vod_copy(p, suffix->data, suffix->len);

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_tracks_spec: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
m3u8_builder_build_iframe_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	hls_mpegts_muxer_conf_t* muxer_conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	vod_str_t* result)
{
	hls_encryption_params_t encryption_params;
	write_segment_context_t ctx;
	segment_durations_t segment_durations;
	segmenter_conf_t* segmenter_conf = media_set->segmenter_conf;
	size_t iframe_length;
	size_t result_size;
	uint64_t duration_millis;
	vod_status_t rc; 

	// iframes list is not supported with encryption, since:
	// 1. AES-128 - the IV of each key frame is not known in advance
	// 2. SAMPLE-AES - the layout of the TS files is not known in advance due to emulation prevention
	encryption_params.type = HLS_ENC_NONE;
	encryption_params.key = NULL;
	encryption_params.iv = NULL;

	// build the required tracks string
	rc = m3u8_builder_build_tracks_spec(
		request_context,
		media_set,
		&m3u8_ts_suffix,
		&ctx.name_suffix);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get segment durations
	if (segmenter_conf->align_to_key_frames)
	{
		rc = segmenter_get_segment_durations_accurate(
			request_context,
			segmenter_conf,
			media_set,
			NULL,
			MEDIA_TYPE_NONE,
			&segment_durations);
	}
	else
	{
		rc = segmenter_get_segment_durations_estimate(
			request_context,
			segmenter_conf,
			media_set,
			NULL,
			MEDIA_TYPE_NONE,
			&segment_durations);
	}

	if (rc != VOD_OK)
	{
		return rc;
	}

	duration_millis = segment_durations.duration;
	iframe_length = sizeof("#EXTINF:.000,\n") - 1 + vod_get_int_print_len(vod_div_ceil(duration_millis, 1000)) +
		sizeof(byte_range_tag_format) + VOD_INT32_LEN + vod_get_int_print_len(MAX_FRAME_SIZE) - (sizeof("%uD%uD") - 1) +
		base_url->len + conf->segment_file_name_prefix.len + 1 + vod_get_int_print_len(segment_durations.segment_count) + ctx.name_suffix.len;

	result_size =
		conf->iframes_m3u8_header_len +
		iframe_length * media_set->sequences[0].video_key_frame_count +
		sizeof(m3u8_footer);

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_iframe_playlist: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// fill out the buffer
	ctx.p = vod_copy(result->data, conf->iframes_m3u8_header, conf->iframes_m3u8_header_len);

	if (media_set->sequences[0].video_key_frame_count > 0)
	{
		ctx.base_url = base_url;
		ctx.segment_file_name_prefix = &conf->segment_file_name_prefix;
	
		rc = hls_muxer_simulate_get_iframes(
			request_context,
			&segment_durations, 
			muxer_conf,
			&encryption_params,
			media_set, 
			m3u8_builder_append_iframe_string, 
			&ctx);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	ctx.p = vod_copy(ctx.p, m3u8_footer, sizeof(m3u8_footer) - 1);
	result->len = ctx.p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_iframe_playlist: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

#if (NGX_HAVE_OPENSSL_EVP)
static vod_status_t
m3u8_builder_write_psshs(
	request_context_t* request_context,
	drm_info_t* drm_info,
	vod_str_t* result)
{
	drm_system_info_t* cur_info;
	size_t result_size = 0;
	u_char* p;

	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		result_size += ATOM_HEADER_SIZE + sizeof(pssh_atom_t) + cur_info->data.len;
	}

	p = vod_alloc(request_context->pool, result_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_write_psshs: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->data = p;

	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		p = edash_packager_write_pssh(p, cur_info);
	}

	result->len = p - result->data;

	if (result->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_write_psshs: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}
#endif // NGX_HAVE_OPENSSL_EVP

vod_status_t
m3u8_builder_build_index_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* segments_base_url,
	hls_encryption_params_t* encryption_params,
	vod_uint_t container_format,
	media_set_t* media_set,
	vod_str_t* result)
{
	segment_durations_t segment_durations;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item;
	hls_encryption_type_t encryption_type;
	segmenter_conf_t* segmenter_conf = media_set->segmenter_conf;
	vod_str_t name_suffix;
	vod_str_t extinf;
	vod_str_t* suffix;
	uint32_t conf_max_segment_duration;
	uint64_t max_segment_duration;
	uint64_t duration_millis;
	uint32_t segment_index;
	uint32_t last_segment_index;
	uint32_t clip_index = 0;
	uint32_t scale;
	size_t segment_length;
	size_t result_size;
	vod_status_t rc;
	u_char* p;

#if (NGX_HAVE_OPENSSL_EVP)
	vod_str_t base64;
	vod_str_t psshs;
#endif // NGX_HAVE_OPENSSL_EVP

	// build the required tracks string
	if (media_set->track_count[MEDIA_TYPE_VIDEO] != 0 || media_set->track_count[MEDIA_TYPE_AUDIO] != 0)
	{
		encryption_type = encryption_params->type;

		if (container_format == HLS_CONTAINER_MPEGTS)
		{
			suffix = &m3u8_ts_suffix;
		}
		else
		{
			suffix = &m3u8_m4s_suffix;
		}
	}
	else
	{
		encryption_type = HLS_ENC_NONE;
		container_format = HLS_CONTAINER_MPEGTS;		// do not output any fmp4-specific tags
		suffix = &m3u8_vtt_suffix;
	}

	rc = m3u8_builder_build_tracks_spec(
		request_context,
		media_set,
		suffix,
		&name_suffix);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the segment durations
	rc = segmenter_conf->get_segment_durations(
		request_context,
		segmenter_conf,
		media_set,
		NULL,
		MEDIA_TYPE_NONE,
		&segment_durations);
	if (rc != VOD_OK)
	{
		return rc;
	}
	last_item = segment_durations.items + segment_durations.item_count;

	// get the required buffer length
	duration_millis = segment_durations.duration;
	last_segment_index = last_item[-1].segment_index + last_item[-1].repeat_count;
	segment_length = sizeof("#EXTINF:.000,\n") - 1 + vod_get_int_print_len(vod_div_ceil(duration_millis, 1000)) +
		segments_base_url->len + conf->segment_file_name_prefix.len + 1 + vod_get_int_print_len(last_segment_index) + name_suffix.len;

	result_size =
		sizeof(M3U8_HEADER_PART1) + VOD_INT64_LEN +
		sizeof(M3U8_HEADER_EVENT) +
		sizeof(M3U8_HEADER_PART2) + VOD_INT64_LEN + VOD_INT32_LEN +
		segment_length * segment_durations.segment_count +
		segment_durations.discontinuities * (sizeof(m3u8_discontinuity) - 1) +
		(sizeof(m3u8_map_prefix) - 1 +
		 base_url->len +
		 conf->init_file_name_prefix.len +
		 sizeof(m3u8_clip_index) - 1 + VOD_INT32_LEN +
		 name_suffix.len +
		 sizeof(m3u8_map_suffix) - 1) *
		(segment_durations.discontinuities + 1) +
		sizeof(m3u8_footer);

	if (encryption_type != HLS_ENC_NONE)
	{
		result_size +=
			sizeof(encryption_key_tag_method) - 1 +
			sizeof(encryption_type_sample_aes_cenc) - 1 +
			sizeof(encryption_key_tag_uri) - 1 + 
			2;			// '"', '\n'

		if (encryption_params->key_uri.len != 0)
		{
			result_size += encryption_params->key_uri.len;
		}
#if (NGX_HAVE_OPENSSL_EVP)
		else if (encryption_params->type == HLS_ENC_SAMPLE_AES_CENC)
		{
			rc = m3u8_builder_write_psshs(
				request_context,
				media_set->sequences[0].drm_info,
				&psshs);
			if (rc != VOD_OK)
			{
				return rc;
			}

			result_size += sizeof(sample_aes_cenc_uri_prefix) + vod_base64_encoded_length(psshs.len);
		}
#endif // NGX_HAVE_OPENSSL_EVP
		else
		{
			result_size += base_url->len +
				conf->encryption_key_file_name.len +
				sizeof("-f") - 1 + VOD_INT32_LEN +
				sizeof(encryption_key_extension) - 1;
		}

		if (encryption_params->return_iv)
		{
			result_size +=
				sizeof(encryption_key_tag_iv) - 1 +
				sizeof(encryption_params->iv_buf) * 2;
		}

		if (conf->encryption_key_format.len != 0)
		{
			result_size +=
				sizeof(encryption_key_tag_key_format) +				// '"'
				conf->encryption_key_format.len;
		}

		if (conf->encryption_key_format_versions.len != 0)
		{
			result_size +=
				sizeof(encryption_key_tag_key_format_versions) +	// '"'
				conf->encryption_key_format_versions.len;
		}
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_index_playlist: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// find the max segment duration
	max_segment_duration = 0;
	for (cur_item = segment_durations.items; cur_item < last_item; cur_item++)
	{
		if (cur_item->duration > max_segment_duration)
		{
			max_segment_duration = cur_item->duration;
		}
	}

	// Note: scaling first to 'scale' so that target duration will always be round(max(manifest durations))
	scale = conf->m3u8_version >= 3 ? 1000 : 1;
	max_segment_duration = rescale_time(max_segment_duration, segment_durations.timescale, scale);
	max_segment_duration = rescale_time(max_segment_duration, scale, 1);

	// make sure segment duration is not lower than the value set in the conf
	conf_max_segment_duration = (segmenter_conf->max_segment_duration + 500) / 1000;
	if (conf_max_segment_duration > max_segment_duration)
	{
		max_segment_duration = conf_max_segment_duration;
	}

	// write the header
	p = vod_sprintf(
		result->data,
		M3U8_HEADER_PART1,
		max_segment_duration);

	if (media_set->type == MEDIA_SET_VOD)
	{
		p = vod_copy(p, M3U8_HEADER_VOD, sizeof(M3U8_HEADER_VOD) - 1);
	}
	else if (media_set->is_live_event)
	{
		p = vod_copy(p, M3U8_HEADER_EVENT, sizeof(M3U8_HEADER_EVENT) - 1);
	}

	if (encryption_type != HLS_ENC_NONE)
	{
		p = vod_copy(p, encryption_key_tag_method, sizeof(encryption_key_tag_method) - 1);
		switch (encryption_type)
		{
		case HLS_ENC_SAMPLE_AES:
			p = vod_copy(p, encryption_type_sample_aes, sizeof(encryption_type_sample_aes) - 1);
			break;

		case HLS_ENC_SAMPLE_AES_CENC:
			p = vod_copy(p, encryption_type_sample_aes_cenc, sizeof(encryption_type_sample_aes_cenc) - 1);
			break;

		default:		// HLS_ENC_AES_128
			p = vod_copy(p, encryption_type_aes_128, sizeof(encryption_type_aes_128) - 1);
			break;
		}

		// uri
		p = vod_copy(p, encryption_key_tag_uri, sizeof(encryption_key_tag_uri) - 1);
		if (encryption_params->key_uri.len != 0)
		{
			p = vod_copy(p, encryption_params->key_uri.data, encryption_params->key_uri.len);
		}
#if (NGX_HAVE_OPENSSL_EVP)
		else if (encryption_params->type == HLS_ENC_SAMPLE_AES_CENC)
		{
			base64.data = vod_copy(p, sample_aes_cenc_uri_prefix, sizeof(sample_aes_cenc_uri_prefix) - 1);
			vod_encode_base64(&base64, &psshs);
			p = base64.data + base64.len;
		}
#endif // NGX_HAVE_OPENSSL_EVP
		else
		{
			p = vod_copy(p, base_url->data, base_url->len);
			p = vod_copy(p, conf->encryption_key_file_name.data, conf->encryption_key_file_name.len);
			if (media_set->has_multi_sequences)
			{
				p = vod_sprintf(p, "-f%uD", media_set->sequences->index + 1);
			}
			p = vod_copy(p, encryption_key_extension, sizeof(encryption_key_extension) - 1);
		}
		*p++ = '"';

		// iv
		if (encryption_params->return_iv)
		{
			p = vod_copy(p, encryption_key_tag_iv, sizeof(encryption_key_tag_iv) - 1);
			p = vod_append_hex_string(p, encryption_params->iv, sizeof(encryption_params->iv_buf));
		}

		// keyformat
		if (conf->encryption_key_format.len != 0)
		{
			p = vod_copy(p, encryption_key_tag_key_format, sizeof(encryption_key_tag_key_format) - 1);
			p = vod_copy(p, conf->encryption_key_format.data, conf->encryption_key_format.len);
			*p++ = '"';
		}

		// keyformatversions
		if (conf->encryption_key_format_versions.len != 0)
		{
			p = vod_copy(p, encryption_key_tag_key_format_versions, sizeof(encryption_key_tag_key_format_versions) - 1);
			p = vod_copy(p, conf->encryption_key_format_versions.data, conf->encryption_key_format_versions.len);
			*p++ = '"';
		}

		*p++ = '\n';
	}

	p = vod_sprintf(
		p,
		M3U8_HEADER_PART2,
		container_format == HLS_CONTAINER_FMP4 ? 6 : conf->m3u8_version, 
		segment_durations.items[0].segment_index + 1);

	if (container_format == HLS_CONTAINER_FMP4)
	{
		p = vod_copy(p, m3u8_map_prefix, sizeof(m3u8_map_prefix) - 1);
		p = vod_copy(p, base_url->data, base_url->len);
		p = vod_copy(p, conf->init_file_name_prefix.data, conf->init_file_name_prefix.len);
		if (media_set->use_discontinuity && 
			media_set->initial_clip_index != INVALID_CLIP_INDEX)
		{
			clip_index = media_set->initial_clip_index + 1;
			p = vod_sprintf(p, m3u8_clip_index, clip_index++);
		}
		p = vod_copy(p, name_suffix.data, name_suffix.len - suffix->len);
		p = vod_copy(p, m3u8_map_suffix, sizeof(m3u8_map_suffix) - 1);
	}

	// write the segments
	for (cur_item = segment_durations.items; cur_item < last_item; cur_item++)
	{
		segment_index = cur_item->segment_index;
		last_segment_index = segment_index + cur_item->repeat_count;

		if (cur_item->discontinuity)
		{
			p = vod_copy(p, m3u8_discontinuity, sizeof(m3u8_discontinuity) - 1);
			if (container_format == HLS_CONTAINER_FMP4 && 
				cur_item > segment_durations.items &&
				media_set->initial_clip_index != INVALID_CLIP_INDEX)
			{
				p = vod_copy(p, m3u8_map_prefix, sizeof(m3u8_map_prefix) - 1);
				p = vod_copy(p, base_url->data, base_url->len);
				p = vod_copy(p, conf->init_file_name_prefix.data, conf->init_file_name_prefix.len);
				p = vod_sprintf(p, m3u8_clip_index, clip_index++);
				p = vod_copy(p, name_suffix.data, name_suffix.len - suffix->len);
				p = vod_copy(p, m3u8_map_suffix, sizeof(m3u8_map_suffix) - 1);
			}
		}

		// ignore zero duration segments (caused by alignment to keyframes)
		if (cur_item->duration == 0)
		{
			continue;
		}

		// write the first segment
		extinf.data = p;
		p = m3u8_builder_append_extinf_tag(p, rescale_time(cur_item->duration, segment_durations.timescale, scale), scale);
		extinf.len = p - extinf.data;
		p = m3u8_builder_append_segment_name(p, segments_base_url, &conf->segment_file_name_prefix, segment_index, &name_suffix);
		segment_index++;

		// write any additional segments
		for (; segment_index < last_segment_index; segment_index++)
		{
			p = vod_copy(p, extinf.data, extinf.len);
			p = m3u8_builder_append_segment_name(p, segments_base_url, &conf->segment_file_name_prefix, segment_index, &name_suffix);
		}
	}

	// write the footer
	if (media_set->presentation_end)
	{
		p = vod_copy(p, m3u8_footer, sizeof(m3u8_footer) - 1);
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_index_playlist: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

static uint32_t
m3u8_builder_get_audio_codec_count(
	adaptation_sets_t* adaptation_sets,
	media_track_t** audio_codec_tracks)
{
	adaptation_set_t* last_adaptation_set;
	adaptation_set_t* cur_adaptation_set;
	media_track_t* cur_track;
	uint32_t seen_codecs = 0;
	uint32_t codec_flag;
	uint32_t count = 0;

	cur_adaptation_set = adaptation_sets->first_by_type[MEDIA_TYPE_AUDIO];
	last_adaptation_set = cur_adaptation_set + adaptation_sets->count[MEDIA_TYPE_AUDIO];
	for (; cur_adaptation_set < last_adaptation_set; cur_adaptation_set++)
	{
		cur_track = cur_adaptation_set->first[0];
		codec_flag = 1 << (cur_track->media_info.codec_id - VOD_CODEC_ID_AUDIO);
		if ((seen_codecs & codec_flag) != 0)
		{
			continue;
		}

		seen_codecs |= codec_flag;
		*audio_codec_tracks++ = cur_track;
		count++;
	}

	return count;
}

static u_char*
m3u8_builder_append_index_url(
	u_char* p,
	vod_str_t* prefix,
	media_set_t* media_set,
	media_track_t** tracks,
	vod_str_t* base_url)
{
	media_track_t* main_track;
	media_track_t* sub_track;
	uint32_t media_type;
	bool_t write_sequence_index;

	// get the main track and sub track
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		if (tracks[media_type] != NULL)
		{
			break;
		}
	}
	main_track = tracks[media_type];
	sub_track = media_type == MEDIA_TYPE_VIDEO ? tracks[MEDIA_TYPE_AUDIO] : NULL;

	write_sequence_index = media_set->has_multi_sequences;
	if (base_url->len != 0)
	{
		// absolute url only
		p = vod_copy(p, base_url->data, base_url->len);
		if (p[-1] != '/')
		{
			if (main_track->file_info.uri.len != 0 &&
				(sub_track == NULL || vod_str_equals(main_track->file_info.uri, sub_track->file_info.uri)))
			{
				p = vod_copy(p, main_track->file_info.uri.data, main_track->file_info.uri.len);
				write_sequence_index = FALSE;		// no need to pass the sequence index since we have a direct uri
			}
			else
			{
				p = vod_copy(p, media_set->uri.data, media_set->uri.len);
			}
			*p++ = '/';
		}
	}

	p = vod_copy(p, prefix->data, prefix->len);
	p = manifest_utils_append_tracks_spec(p, tracks, MEDIA_TYPE_COUNT, write_sequence_index);
	p = vod_copy(p, m3u8_url_suffix, sizeof(m3u8_url_suffix) - 1);

	return p;
}

static size_t
m3u8_builder_closed_captions_get_size(
	media_set_t* media_set,
	request_context_t* request_context)
{
	media_closed_captions_t* closed_captions;
	size_t result = 0;
	size_t base;

	base =
		sizeof(M3U8_EXT_MEDIA_BASE) - 1 +
		sizeof(M3U8_EXT_MEDIA_TYPE_CLOSED_CAPTIONS) - 1 +
		sizeof(M3U8_EXT_MEDIA_GROUP_ID_CLOSED_CAPTIONS) - 1 + VOD_INT32_LEN +
		sizeof(M3U8_EXT_MEDIA_LANG) - 1 +
		sizeof(M3U8_EXT_MEDIA_INSTREAM_ID) - 1 +
		sizeof(M3U8_EXT_MEDIA_DEFAULT) - 1;

	for (closed_captions = media_set->closed_captions; closed_captions < media_set->closed_captions_end; closed_captions++)
	{
		result += base + closed_captions->id.len + closed_captions->label.len + closed_captions->language.len + sizeof("\n") - 1;
	}

	return result + sizeof("\n") - 1;
}

static u_char*
m3u8_builder_closed_captions_write(
	u_char* p,
	media_set_t* media_set)
{
	media_closed_captions_t* closed_captions;
	uint32_t index = 0;
	bool_t is_default;

	for (closed_captions = media_set->closed_captions; closed_captions < media_set->closed_captions_end; closed_captions++)
	{
		p = vod_sprintf(p, M3U8_EXT_MEDIA_BASE,
			M3U8_EXT_MEDIA_TYPE_CLOSED_CAPTIONS,
			M3U8_EXT_MEDIA_GROUP_ID_CLOSED_CAPTIONS,
			index,
			&closed_captions->label);
		
		if (closed_captions->language.len != 0)
		{
			p = vod_sprintf(p, M3U8_EXT_MEDIA_LANG, &closed_captions->language);
		}

		is_default = closed_captions->is_default;
		if (is_default < 0)
		{
			is_default = closed_captions == media_set->closed_captions;
		}

		if (is_default)
		{
			p = vod_copy(p, M3U8_EXT_MEDIA_DEFAULT, sizeof(M3U8_EXT_MEDIA_DEFAULT) - 1);
		}
		else 
		{
			p = vod_copy(p, M3U8_EXT_MEDIA_NON_DEFAULT, sizeof(M3U8_EXT_MEDIA_NON_DEFAULT) - 1);
		}

		p = vod_sprintf(p, M3U8_EXT_MEDIA_INSTREAM_ID, (vod_str_t*) &closed_captions->id);

		*p++ = '\n';
	}

	*p++ = '\n';

	return p;
}

static size_t
m3u8_builder_ext_x_media_tags_get_size(
	adaptation_sets_t* adaptation_sets,
	vod_str_t* base_url,
	size_t base_url_len,
	media_set_t* media_set,
	uint32_t media_type)
{
	adaptation_set_t* first_adaptation_set;
	adaptation_set_t* last_adaptation_set;
	adaptation_set_t* adaptation_set;
	media_track_t* cur_track;
	size_t label_len;
	size_t result;

	result =
		sizeof("\n\n") - 1 +
		(sizeof(M3U8_EXT_MEDIA_BASE) - 1 + VOD_INT32_LEN +
		sizeof(M3U8_EXT_MEDIA_TYPE_SUBTITLES) - 1 +
		sizeof(M3U8_EXT_MEDIA_GROUP_ID_AUDIO) - 1 +
		sizeof(M3U8_EXT_MEDIA_LANG) - 1 +
		sizeof(M3U8_EXT_MEDIA_DEFAULT) - 1 +
		sizeof(M3U8_EXT_MEDIA_AUTOSELECT) - 1 +
		sizeof(M3U8_EXT_MEDIA_CHARACTERISTICS) - 1 + MAX_CHARACTERISTICS_SIZE +
		sizeof(M3U8_EXT_MEDIA_FORCED) - 1 +
		sizeof(M3U8_EXT_MEDIA_CHANNELS) - 1 + VOD_INT32_LEN +
		sizeof(M3U8_EXT_MEDIA_URI) - 1 +
		base_url_len +
		sizeof("\"\n") - 1) * (adaptation_sets->count[media_type]);

	first_adaptation_set = adaptation_sets->first_by_type[media_type];
	last_adaptation_set = first_adaptation_set + adaptation_sets->count[media_type];
	for (adaptation_set = first_adaptation_set; adaptation_set < last_adaptation_set; adaptation_set++)
	{
		cur_track = adaptation_set->first[0];

		label_len = cur_track->media_info.tags.label.len;
		result += vod_max(label_len, default_label.len) + cur_track->media_info.tags.lang_str.len;

		if (base_url->len != 0)
		{
			result += vod_max(cur_track->file_info.uri.len, media_set->uri.len);
		}
	}

	return result;
}

static u_char*
m3u8_builder_ext_x_media_tags_write(
	u_char* p,
	adaptation_sets_t* adaptation_sets,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	uint32_t media_type)
{
	adaptation_set_t* first_adaptation_set;
	adaptation_set_t* last_adaptation_set;
	adaptation_set_t* adaptation_set;
	media_track_t* tracks[MEDIA_TYPE_COUNT];
	vod_str_t* label;
	uint32_t group_index;
	bool_t is_default;
	char* group_id;
	char* type;

	switch (media_type)
	{
	case MEDIA_TYPE_AUDIO:
		type = M3U8_EXT_MEDIA_TYPE_AUDIO;
		group_id = M3U8_EXT_MEDIA_GROUP_ID_AUDIO;
		break;

	case MEDIA_TYPE_SUBTITLE:
		type = M3U8_EXT_MEDIA_TYPE_SUBTITLES;
		group_id = M3U8_EXT_MEDIA_GROUP_ID_SUBTITLES;
		break;

	default:
		return p;	// can't happen, just to avoid the warning
	}

	*p++ = '\n';

	vod_memzero(tracks, sizeof(tracks));
	tracks[MEDIA_TYPE_VIDEO] = NULL;
	first_adaptation_set = adaptation_sets->first_by_type[media_type];
	last_adaptation_set = first_adaptation_set + adaptation_sets->count[media_type];
	for (adaptation_set = first_adaptation_set; adaptation_set < last_adaptation_set; adaptation_set++)
	{
		// take only the first track
		tracks[media_type] = adaptation_set->first[0];

		// output EXT-X-MEDIA
		if (media_type == MEDIA_TYPE_AUDIO)
		{
			group_index = tracks[media_type]->media_info.codec_id - VOD_CODEC_ID_AUDIO;
		}
		else
		{
			group_index = 0;
		}

		label = &tracks[media_type]->media_info.tags.label;
		if (label->len == 0)
		{
			label = &default_label;
		}

		p = vod_sprintf(p, M3U8_EXT_MEDIA_BASE,
			type,
			group_id,
			group_index,
			label);

		if (tracks[media_type]->media_info.tags.lang_str.len > 0)
		{
			p = vod_sprintf(p, M3U8_EXT_MEDIA_LANG,
				&tracks[media_type]->media_info.tags.lang_str);
		}

		is_default = tracks[media_type]->media_info.tags.is_default;
		if (is_default < 0)
		{
			is_default = adaptation_set == first_adaptation_set;
		}

		if (is_default)
		{
			p = vod_copy(p, M3U8_EXT_MEDIA_DEFAULT, sizeof(M3U8_EXT_MEDIA_DEFAULT) - 1);
		}

		if (tracks[media_type]->media_info.tags.is_autoselect > 0)
		{
			p = vod_copy(p, M3U8_EXT_MEDIA_AUTOSELECT, sizeof(M3U8_EXT_MEDIA_AUTOSELECT) - 1);
		}

		if (tracks[media_type]->media_info.tags.characteristics.len != 0)
		{
			p = vod_sprintf(p, M3U8_EXT_MEDIA_CHARACTERISTICS,
				&tracks[media_type]->media_info.tags.characteristics);
		}

		if (tracks[media_type]->media_info.tags.is_forced > 0 && media_type == MEDIA_TYPE_SUBTITLE)
		{
			p = vod_copy(p, M3U8_EXT_MEDIA_FORCED, sizeof(M3U8_EXT_MEDIA_FORCED) - 1);
		}

		if (media_type == MEDIA_TYPE_AUDIO)
		{
			p = vod_sprintf(p, M3U8_EXT_MEDIA_CHANNELS, 
				(uint32_t)tracks[media_type]->media_info.u.audio.channels);
		}

		p = vod_copy(p, M3U8_EXT_MEDIA_URI, sizeof(M3U8_EXT_MEDIA_URI) - 1);

		p = m3u8_builder_append_index_url(
			p,
			&conf->index_file_name_prefix,
			media_set,
			tracks,
			base_url);

		*p++ = '"';
		*p++ = '\n';
	}

	*p++ = '\n';

	return p;
}

static u_char*
m3u8_builder_write_video_range(u_char* p, media_info_t* media_info)
{
	if (media_info->format == FORMAT_DVH1)
	{
		p = vod_copy(p, M3U8_VIDEO_RANGE_PQ, sizeof(M3U8_VIDEO_RANGE_PQ) - 1);
		return p;
	}

	switch (media_info->u.video.transfer_characteristics)
	{
	case 1:
		p = vod_copy(p, M3U8_VIDEO_RANGE_SDR, sizeof(M3U8_VIDEO_RANGE_SDR) - 1);
		break;

	case 16:
	case 18:
		p = vod_copy(p, M3U8_VIDEO_RANGE_PQ, sizeof(M3U8_VIDEO_RANGE_PQ) - 1);
		break;
	}

	return p;
}

static u_char*
m3u8_builder_write_variants(
	u_char* p,
	adaptation_sets_t* adaptation_sets,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	media_track_t* group_audio_track)
{
	adaptation_set_t* adaptation_set = adaptation_sets->first;
	media_track_t** cur_track_ptr;
	media_track_t* tracks[MEDIA_TYPE_COUNT];
	media_info_t* video = NULL;
	media_info_t* audio = NULL;
	uint32_t bitrate;
	uint32_t avg_bitrate;
	uint32_t muxed_tracks = adaptation_set->type == ADAPTATION_TYPE_MUXED ? MEDIA_TYPE_COUNT : 1;

	vod_memzero(tracks, sizeof(tracks));

	for (cur_track_ptr = adaptation_set->first;
		cur_track_ptr < adaptation_set->last;
		cur_track_ptr += muxed_tracks)
	{
		// get the audio / video tracks
		if (muxed_tracks == MEDIA_TYPE_COUNT)
		{
			tracks[MEDIA_TYPE_VIDEO] = cur_track_ptr[MEDIA_TYPE_VIDEO];
			tracks[MEDIA_TYPE_AUDIO] = cur_track_ptr[MEDIA_TYPE_AUDIO];
		}
		else
		{
			// Note: this is ok because the adaptation types enum is aligned with media types
			tracks[adaptation_set->type] = cur_track_ptr[0];
		}

		// output EXT-X-STREAM-INF
		if (tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			video = &tracks[MEDIA_TYPE_VIDEO]->media_info;
			bitrate = video->bitrate;
			avg_bitrate = video->avg_bitrate;
			if (group_audio_track != NULL)
			{
				audio = &group_audio_track->media_info;
			}
			else if (tracks[MEDIA_TYPE_AUDIO] != NULL)
			{
				audio = &tracks[MEDIA_TYPE_AUDIO]->media_info;
			}
			else
			{
				audio = NULL;
			}

			if (audio != NULL)
			{
				bitrate += audio->bitrate;
				if (avg_bitrate != 0)
				{
					avg_bitrate += audio->avg_bitrate;
				}
			}

			p = vod_sprintf(p, m3u8_stream_inf_video,
				bitrate,
				(uint32_t)video->u.video.width,
				(uint32_t)video->u.video.height,
				(uint32_t)(video->timescale / video->min_frame_duration),
				(uint32_t)((((uint64_t)video->timescale * 1000) / video->min_frame_duration) % 1000),
				&video->codec_name);
			if (audio != NULL)
			{
				*p++ = ',';
				p = vod_copy(p, audio->codec_name.data, audio->codec_name.len);
			}
		}
		else
		{
			if (group_audio_track != NULL)
			{
				audio = &group_audio_track->media_info;
			}
			else
			{
				audio = &tracks[MEDIA_TYPE_AUDIO]->media_info;
			}

			avg_bitrate = audio->avg_bitrate;
			p = vod_sprintf(p, m3u8_stream_inf_audio, audio->bitrate, &audio->codec_name);
		}

		*p++ = '\"';

		if (avg_bitrate != 0)
		{
			p = vod_sprintf(p, m3u8_average_bandwidth, avg_bitrate);
		}

		if (tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			p = m3u8_builder_write_video_range(p, video);
		}

		if (adaptation_sets->count[ADAPTATION_TYPE_AUDIO] > 0 && adaptation_sets->total_count > 1)
		{
			p = vod_sprintf(p, M3U8_STREAM_TAG_AUDIO, group_audio_track->media_info.codec_id - VOD_CODEC_ID_AUDIO);
		}
		if (adaptation_sets->count[ADAPTATION_TYPE_SUBTITLE] > 0)
		{
			p = vod_sprintf(p, M3U8_STREAM_TAG_SUBTITLES, 0);
		}
		if (media_set->closed_captions < media_set->closed_captions_end)
		{
			p = vod_sprintf(p, M3U8_STREAM_TAG_CLOSED_CAPTIONS, 0);
		} 
		else if (media_set->closed_captions != NULL)
		{
			p = vod_copy(p, M3U8_STREAM_TAG_NO_CLOSED_CAPTIONS, sizeof(M3U8_STREAM_TAG_NO_CLOSED_CAPTIONS) - 1);
		}
		*p++ = '\n';

		// output the url
		p = m3u8_builder_append_index_url(
			p,
			&conf->index_file_name_prefix,
			media_set,
			tracks,
			base_url);

		*p++ = '\n';
	}

	*p++ = '\n';

	return p;
}

static u_char*
m3u8_builder_write_iframe_variants(
	u_char* p,
	adaptation_set_t* adaptation_set,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set)
{
	media_track_t** cur_track_ptr;
	media_track_t* tracks[MEDIA_TYPE_COUNT];
	media_info_t* video;
	uint32_t muxed_tracks = adaptation_set->type == ADAPTATION_TYPE_MUXED ? MEDIA_TYPE_COUNT : 1;

	vod_memzero(tracks, sizeof(tracks));

	for (cur_track_ptr = adaptation_set->first;
		cur_track_ptr < adaptation_set->last;
		cur_track_ptr += muxed_tracks)
	{
		// get the audio / video tracks
		if (muxed_tracks == MEDIA_TYPE_COUNT)
		{
			tracks[MEDIA_TYPE_VIDEO] = cur_track_ptr[MEDIA_TYPE_VIDEO];
			tracks[MEDIA_TYPE_AUDIO] = cur_track_ptr[MEDIA_TYPE_AUDIO];
		}
		else
		{
			// Note: this is ok because the adaptation types enum is aligned with media types
			tracks[adaptation_set->type] = cur_track_ptr[0];
		}

		if (tracks[MEDIA_TYPE_VIDEO] == NULL)
		{
			continue;
		}

		video = &tracks[MEDIA_TYPE_VIDEO]->media_info;
		if (conf->container_format == HLS_CONTAINER_AUTO && 
			video->codec_id != VOD_CODEC_ID_AVC)
		{
			continue;
		}

		if (video->u.video.key_frame_bitrate == 0 ||
			!mp4_to_annexb_simulation_supported(video))
		{
			continue;
		}

		p = vod_sprintf(p, m3u8_iframe_stream_inf,
			video->u.video.key_frame_bitrate,
			(uint32_t)video->u.video.width,
			(uint32_t)video->u.video.height,
			&video->codec_name);

		// Note: while it is possible to use only the video track here, sending the audio
		//		makes the iframe list reference the same segments as the media playlist
		p = m3u8_builder_append_index_url(
			p,
			&conf->iframes_file_name_prefix,
			media_set,
			tracks,
			base_url);
		*p++ = '\"';

		p = m3u8_builder_write_video_range(p, video);

		*p++ = '\n';
	}

	return p;
}

vod_status_t
m3u8_builder_build_master_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_uint_t encryption_method,
	vod_str_t* base_url,
	media_set_t* media_set,
	vod_str_t* result)
{
	adaptation_sets_t adaptation_sets;
	media_track_t** last_audio_codec_track;
	media_track_t** cur_track_ptr;
	media_track_t* audio_codec_tracks[VOD_CODEC_ID_SUBTITLE - VOD_CODEC_ID_AUDIO];
	media_track_t* cur_track;
	vod_status_t rc;
	uint32_t variant_set_count;
	uint32_t variant_set_size;
	uint32_t muxed_tracks;
	uint32_t flags;
	bool_t iframe_playlist;
	size_t max_video_stream_inf;
	size_t base_url_len;
	size_t result_size;
	u_char* p;
	bool_t alternative_audio;

	// get the adaptations sets
	flags = ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK | ADAPTATION_SETS_FLAG_MULTI_AUDIO_CODEC;
	if (!conf->force_unmuxed_segments && encryption_method != HLS_ENC_SAMPLE_AES_CENC)
	{
		flags |= ADAPTATION_SETS_FLAG_MUXED;
	}

	rc = manifest_utils_get_adaptation_sets(
		request_context, 
		media_set, 
		flags,
		&adaptation_sets);
	if (rc != VOD_OK)
	{
		return rc;
	}

	iframe_playlist = conf->output_iframes_playlist &&
		(media_set->type == MEDIA_SET_VOD || media_set->is_live_event) &&
		media_set->timing.total_count <= 1 &&
		encryption_method == HLS_ENC_NONE &&
		conf->container_format != HLS_CONTAINER_FMP4 &&
		!media_set->audio_filtering_needed &&
		(adaptation_sets.first->type == ADAPTATION_TYPE_MUXED || adaptation_sets.first->type == ADAPTATION_TYPE_VIDEO);

	// get the response size
	base_url_len = base_url->len + 1 + conf->index_file_name_prefix.len +			// 1 = /
		MANIFEST_UTILS_TRACKS_SPEC_MAX_SIZE + sizeof(m3u8_url_suffix) - 1;

	result_size = sizeof(m3u8_header);

	max_video_stream_inf =
		sizeof(m3u8_stream_inf_video) - 1 + 5 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
		MAX_CODEC_NAME_SIZE + 1 +		// 1 = ,
		sizeof(m3u8_average_bandwidth) - 1 + VOD_INT32_LEN +
		sizeof(M3U8_VIDEO_RANGE_SDR) - 1 +
		sizeof("\"\n\n") - 1;

	alternative_audio = adaptation_sets.count[ADAPTATION_TYPE_AUDIO] > 0 && adaptation_sets.total_count > 1;

	if (alternative_audio)
	{
		// alternative audio
		// Note: in case of audio only, the first track is printed twice - once as #EXT-X-STREAM-INF
		//		and once as #EXT-X-MEDIA
		result_size += m3u8_builder_ext_x_media_tags_get_size(
			&adaptation_sets,
			base_url,
			base_url_len,
			media_set,
			MEDIA_TYPE_AUDIO);

		max_video_stream_inf += sizeof(M3U8_STREAM_TAG_AUDIO) - 1 + VOD_INT32_LEN;

		// count the number of audio codecs
		vod_memzero(audio_codec_tracks, sizeof(audio_codec_tracks));
		variant_set_count = m3u8_builder_get_audio_codec_count(
			&adaptation_sets, 
			audio_codec_tracks);
	}
	else
	{
		variant_set_count = 1;
	}

	if (adaptation_sets.count[ADAPTATION_TYPE_SUBTITLE] > 0)
	{
		// subtitles
		result_size += m3u8_builder_ext_x_media_tags_get_size(
			&adaptation_sets,
			base_url,
			base_url_len,
			media_set,
			MEDIA_TYPE_SUBTITLE);

		max_video_stream_inf += sizeof(M3U8_STREAM_TAG_SUBTITLES) - 1 + VOD_INT32_LEN;
	}

	if (media_set->closed_captions < media_set->closed_captions_end)
	{
		result_size += m3u8_builder_closed_captions_get_size(media_set, request_context);

		max_video_stream_inf += sizeof(M3U8_STREAM_TAG_CLOSED_CAPTIONS) - 1;
	}
	else if (media_set->closed_captions != NULL)
	{
		max_video_stream_inf += sizeof(M3U8_STREAM_TAG_NO_CLOSED_CAPTIONS) - 1;
	}


	// variants
	muxed_tracks = adaptation_sets.first->type == ADAPTATION_TYPE_MUXED ? MEDIA_TYPE_COUNT : 1;

	variant_set_size = (max_video_stream_inf +		 // using only video since it's larger than audio
		base_url_len) * adaptation_sets.first->count;

	if (base_url->len != 0)
	{
		for (cur_track_ptr = adaptation_sets.first->first;
			cur_track_ptr < adaptation_sets.first->last;
			cur_track_ptr += muxed_tracks)
		{
			cur_track = cur_track_ptr[0];
			if (cur_track == NULL)
			{
				cur_track = cur_track_ptr[1];
			}

			variant_set_size += vod_max(cur_track->file_info.uri.len, media_set->uri.len);
		}
	}

	result_size += variant_set_size * variant_set_count;

	// iframe playlist
	if (iframe_playlist)
	{
		result_size +=
			(sizeof(m3u8_iframe_stream_inf) - 1 + 3 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE + sizeof("\"\n\n") - 1 +
				base_url_len - conf->index_file_name_prefix.len + conf->iframes_file_name_prefix.len + 
				sizeof(M3U8_VIDEO_RANGE_SDR) - 1) * adaptation_sets.first->count;
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_master_playlist: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	// write the header
	p = vod_copy(result->data, m3u8_header, sizeof(m3u8_header) - 1);

	if (alternative_audio)
	{
		// output alternative audio 
		p = m3u8_builder_ext_x_media_tags_write(
			p,
			&adaptation_sets,
			conf,
			base_url,
			media_set,
			MEDIA_TYPE_AUDIO);
	}

	if (adaptation_sets.count[ADAPTATION_TYPE_SUBTITLE] > 0)
	{
		// output subtitles
		p = m3u8_builder_ext_x_media_tags_write(
			p,
			&adaptation_sets,
			conf,
			base_url,
			media_set,
			MEDIA_TYPE_SUBTITLE);
	}

	if (media_set->closed_captions < media_set->closed_captions_end)
	{
		p = m3u8_builder_closed_captions_write(p, media_set);
	}

	// output variants
	if (variant_set_count > 1)
	{
		last_audio_codec_track = audio_codec_tracks + variant_set_count;
		for (cur_track_ptr = audio_codec_tracks;
			cur_track_ptr < last_audio_codec_track;
			cur_track_ptr++)
		{
			p = m3u8_builder_write_variants(
				p,
				&adaptation_sets,
				conf,
				base_url,
				media_set,
				*cur_track_ptr);
		}
	}
	else
	{
		p = m3u8_builder_write_variants(
			p,
			&adaptation_sets,
			conf,
			base_url,
			media_set,
			alternative_audio ? adaptation_sets.first_by_type[ADAPTATION_TYPE_AUDIO]->first[0] : NULL);
	}

	// iframes
	if (iframe_playlist)
	{
		p = m3u8_builder_write_iframe_variants(
			p,
			adaptation_sets.first,
			conf,
			base_url,
			media_set);
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_master_playlist: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

void 
m3u8_builder_init_config(
	m3u8_config_t* conf, 
	uint32_t max_segment_duration, 
	hls_encryption_type_t encryption_method)
{
	if (encryption_method == HLS_ENC_SAMPLE_AES ||
		encryption_method == HLS_ENC_SAMPLE_AES_CENC ||
		conf->encryption_key_format.len != 0 ||
		conf->encryption_key_format_versions.len != 0)
	{
		conf->m3u8_version = 5;
	}
	else
	{
		conf->m3u8_version = 3;
	}

	conf->iframes_m3u8_header_len = vod_snprintf(
		conf->iframes_m3u8_header,
		sizeof(conf->iframes_m3u8_header) - 1,
		iframes_m3u8_header_format,
		vod_div_ceil(max_segment_duration, 1000)) - conf->iframes_m3u8_header;
}
