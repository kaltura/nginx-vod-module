#include "m3u8_builder.h"
#include "../common.h"

// macros
#define MAX_SEGMENT_COUNT (10 * 1024)			// more than 1 day when using 10 sec segments
#define M3U8_HEADER_PART1 "#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-ALLOW-CACHE:YES\n#EXT-X-PLAYLIST-TYPE:VOD\n"
#define M3U8_HEADER_PART2 "#EXT-X-VERSION:%d\n#EXT-X-MEDIA-SEQUENCE:1\n"


// constants
static const u_char m3u8_header[] = "#EXTM3U\n";
static const u_char m3u8_footer[] = "#EXT-X-ENDLIST\n";
static const char m3u8_stream_inf_video[] = "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%uD,RESOLUTION=%uDx%uD,CODECS=\"%V";
static const char m3u8_stream_inf_audio[] = "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%uD,CODECS=\"%V";
static const u_char m3u8_stream_inf_suffix[] = "\"\n";
static const u_char m3u8_discontinuity[] = "#EXT-X-DISCONTINUITY\n";
static const char byte_range_tag_format[] = "#EXT-X-BYTERANGE:%uD@%uD\n";
static const u_char m3u8_url_suffix[] = ".m3u8\n";

static const char encryption_key_tag_part1[] = "#EXT-X-KEY:METHOD=";
static const char encryption_key_tag_part2[] = ",URI=\"";
static const char encryption_key_tag_part3[] = ".key\"\n";
static const char encryption_type_aes_128[] = "AES-128";
static const char encryption_type_sample_aes[] = "SAMPLE-AES";

// typedefs
typedef struct {
	u_char* p;
	vod_str_t tracks_spec;
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

static vod_status_t
m3u8_builder_build_required_tracks_string(
	request_context_t* request_context, 
	uint32_t sequence_index,
	request_params_t* request_params,
	vod_str_t* tracks_spec)
{
	u_char* p;
	size_t result_size;
	uint32_t i;

	result_size = 0;
	if (request_params->tracks_mask[MEDIA_TYPE_VIDEO] != 0xffffffff)
	{
		result_size += vod_get_number_of_set_bits(request_params->tracks_mask[MEDIA_TYPE_VIDEO]) * (sizeof("-v32") - 1);
	}
	if (request_params->tracks_mask[MEDIA_TYPE_AUDIO] != 0xffffffff)
	{
		result_size += vod_get_number_of_set_bits(request_params->tracks_mask[MEDIA_TYPE_AUDIO]) * (sizeof("-a32") - 1);
	}
	if (sequence_index != INVALID_SEQUENCE_INDEX)
	{
		result_size += sizeof("-f") - 1 + vod_get_int_print_len(sequence_index + 1);
	}

	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_required_tracks_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	tracks_spec->data = p;

	if (sequence_index != INVALID_SEQUENCE_INDEX)
	{
		p = vod_sprintf(p, "-f%uD", sequence_index + 1);
	}

	if (request_params->tracks_mask[MEDIA_TYPE_VIDEO] != 0xffffffff)
	{
		for (i = 0; i < 32; i++)
		{
			if ((request_params->tracks_mask[MEDIA_TYPE_VIDEO] & (1 << i)) == 0)
			{
				continue;
			}

			p = vod_sprintf(p, "-v%uD", i + 1);
		}
	}
	
	if (request_params->tracks_mask[MEDIA_TYPE_AUDIO] != 0xffffffff)
	{
		for (i = 0; i < 32; i++)
		{
			if ((request_params->tracks_mask[MEDIA_TYPE_AUDIO] & (1 << i)) == 0)
			{
				continue;
			}

			p = vod_sprintf(p, "-a%uD", i + 1);
		}
	}

	tracks_spec->len = p - tracks_spec->data;

	if (tracks_spec->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_required_tracks_string: result length %uz exceeded allocated length %uz", 
			tracks_spec->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

static u_char*
m3u8_builder_append_segment_name(
	u_char* p, 
	vod_str_t* base_url,
	vod_str_t* segment_file_name_prefix, 
	uint32_t segment_index, 
	vod_str_t* tracks_spec)
{
	p = vod_copy(p, base_url->data, base_url->len);
	p = vod_copy(p, segment_file_name_prefix->data, segment_file_name_prefix->len);
	*p++ = '-';
	p = vod_sprintf(p, "%uD", segment_index + 1);
	p = vod_copy(p, tracks_spec->data, tracks_spec->len);
	p = vod_copy(p, ".ts\n", sizeof(".ts\n") - 1);
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
		&ctx->tracks_spec);
}

vod_status_t
m3u8_builder_build_iframe_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	hls_muxer_conf_t* muxer_conf,
	vod_str_t* base_url,
	request_params_t* request_params,
	segmenter_conf_t* segmenter_conf,
	media_set_t* media_set,
	vod_str_t* result)
{
	hls_encryption_params_t encryption_params;
	write_segment_context_t ctx;
	segment_durations_t segment_durations;
	size_t iframe_length;
	size_t result_size;
	hls_muxer_state_t muxer_state;
	bool_t simulation_supported;
	uint32_t sequence_index;
	vod_status_t rc; 

	sequence_index = media_set->has_multi_sequences ? media_set->sequences[0].index : INVALID_SEQUENCE_INDEX;

	// iframes list is not supported with encryption, since:
	// 1. AES-128 - the IV of each key frame is not known in advance
	// 2. SAMPLE-AES - the layout of the TS files is not known in advance due to emulation prevention
	encryption_params.type = HLS_ENC_NONE;
	encryption_params.key = NULL;
	encryption_params.iv = NULL;

	// initialize the muxer
	rc = hls_muxer_init(
		&muxer_state, 
		request_context, 
		muxer_conf, 
		&encryption_params, 
		0, 
		media_set, 
		NULL, 
		NULL, 
		NULL, 
		&simulation_supported);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (!simulation_supported)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_iframe_playlist: simulation not supported for this file, cant create iframe playlist");
		return VOD_BAD_REQUEST;
	}

	// build the required tracks string
	rc = m3u8_builder_build_required_tracks_string(
		request_context, 
		sequence_index,
		request_params,
		&ctx.tracks_spec);
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

	iframe_length = sizeof("#EXTINF:.000,\n") - 1 + vod_get_int_print_len(vod_div_ceil(segment_durations.duration_millis, 1000)) +
		sizeof(byte_range_tag_format) + VOD_INT32_LEN + vod_get_int_print_len(MAX_FRAME_SIZE) - (sizeof("%uD%uD") - 1) +
		base_url->len + conf->segment_file_name_prefix.len + 1 + vod_get_int_print_len(segment_durations.segment_count) + ctx.tracks_spec.len + sizeof(".ts\n") - 1;

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
	
		rc = hls_muxer_simulate_get_iframes(&muxer_state, &segment_durations, media_set, m3u8_builder_append_iframe_string, &ctx);
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

vod_status_t
m3u8_builder_build_index_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* segments_base_url,
	request_params_t* request_params,
	hls_encryption_params_t* encryption_params,
	segmenter_conf_t* segmenter_conf,
	media_set_t* media_set,
	vod_str_t* result)
{
	segment_durations_t segment_durations;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item;
	uint32_t sequence_index;
	vod_str_t extinf;
	uint32_t segment_index;
	uint32_t last_segment_index;
	vod_str_t tracks_spec;
	uint32_t scale;
	size_t segment_length;
	size_t result_size;
	vod_status_t rc;
	u_char* p;

	sequence_index = media_set->has_multi_sequences ? media_set->sequences[0].index : INVALID_SEQUENCE_INDEX;

	// build the required tracks string
	rc = m3u8_builder_build_required_tracks_string(
		request_context, 
		sequence_index,
		request_params,
		&tracks_spec);
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

	// get the required buffer length
	segment_length = sizeof("#EXTINF:.000,\n") - 1 + vod_get_int_print_len(vod_div_ceil(segment_durations.duration_millis, 1000)) +
		segments_base_url->len + conf->segment_file_name_prefix.len + 1 + vod_get_int_print_len(segment_durations.segment_count) + tracks_spec.len + sizeof(".ts\n") - 1;

	result_size =
		sizeof(M3U8_HEADER_PART1) + VOD_INT64_LEN + 
		sizeof(M3U8_HEADER_PART2) + VOD_INT64_LEN + 
		segment_length * segment_durations.segment_count +
		segment_durations.discontinuities * (sizeof(m3u8_discontinuity) - 1) +
		sizeof(m3u8_footer);

	if (encryption_params->type != HLS_ENC_NONE)
	{
		result_size +=
			sizeof(encryption_key_tag_part1) - 1 +
			sizeof(encryption_type_sample_aes) - 1 + 
			sizeof(encryption_key_tag_part2) - 1 +
			base_url->len +
			conf->encryption_key_file_name.len + 
			sizeof("-f") - 1 + VOD_INT32_LEN + 
			sizeof(encryption_key_tag_part3) - 1;
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_index_playlist: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// write the header
	p = vod_sprintf(
		result->data,
		M3U8_HEADER_PART1,
		(segmenter_conf->max_segment_duration + 500) / 1000);
	
	if (encryption_params->type != HLS_ENC_NONE)
	{
		p = vod_copy(p, encryption_key_tag_part1, sizeof(encryption_key_tag_part1) - 1);
		switch (encryption_params->type)
		{
		case HLS_ENC_SAMPLE_AES:
			p = vod_copy(p, encryption_type_sample_aes, sizeof(encryption_type_sample_aes) - 1);
			break;

		default:		// HLS_ENC_AES_128
			p = vod_copy(p, encryption_type_aes_128, sizeof(encryption_type_aes_128) - 1);
			break;
		}
		p = vod_copy(p, encryption_key_tag_part2, sizeof(encryption_key_tag_part2) - 1);
		p = vod_copy(p, base_url->data, base_url->len);
		p = vod_copy(p, conf->encryption_key_file_name.data, conf->encryption_key_file_name.len);
		if (sequence_index != INVALID_SEQUENCE_INDEX)
		{
			p = vod_sprintf(p, "-f%uD", sequence_index + 1);
		}
		p = vod_copy(p, encryption_key_tag_part3, sizeof(encryption_key_tag_part3) - 1);
	}

	p = vod_sprintf(
		p,
		M3U8_HEADER_PART2,
		conf->m3u8_version);

	// write the segments
	scale = conf->m3u8_version >= 3 ? 1000 : 1;
	last_item = segment_durations.items + segment_durations.item_count;

	for (cur_item = segment_durations.items; cur_item < last_item; cur_item++)
	{
		segment_index = cur_item->segment_index;
		last_segment_index = segment_index + cur_item->repeat_count;

		if (cur_item->discontinuity)
		{
			p = vod_copy(p, m3u8_discontinuity, sizeof(m3u8_discontinuity) - 1);
		}

		// write the first segment
		extinf.data = p;
		p = m3u8_builder_append_extinf_tag(p, rescale_time(cur_item->duration, segment_durations.timescale, scale), scale);
		extinf.len = p - extinf.data;
		p = m3u8_builder_append_segment_name(p, segments_base_url, &conf->segment_file_name_prefix, segment_index, &tracks_spec);
		segment_index++;

		// write any additional segments
		for (; segment_index < last_segment_index; segment_index++)
		{
			p = vod_copy(p, extinf.data, extinf.len);
			p = m3u8_builder_append_segment_name(p, segments_base_url, &conf->segment_file_name_prefix, segment_index, &tracks_spec);
		}
	}

	// write the footer
	p = vod_copy(p, m3u8_footer, sizeof(m3u8_footer) - 1);

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

vod_status_t
m3u8_builder_build_master_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	vod_str_t* result)
{
	media_track_t** cur_sequence_tracks;
	media_sequence_t* cur_sequence;
	media_track_t* track;
	media_info_t* video;
	media_info_t* audio = NULL;
	uint32_t sequence_index;
	uint32_t bitrate;
	u_char* p;
	size_t max_video_stream_inf;
	size_t result_size;

	// calculate the result size
	max_video_stream_inf = 
		sizeof(m3u8_stream_inf_video) - 1 + 3 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
		MAX_CODEC_NAME_SIZE + 1 +
		sizeof(m3u8_stream_inf_suffix) - 1;
	result_size = 
		sizeof(m3u8_header) + 
		media_set->sequence_count * max_video_stream_inf;		// using only video since it's larger than audio

	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		cur_sequence_tracks = cur_sequence->filtered_clips[0].longest_track;

		track = (cur_sequence_tracks[MEDIA_TYPE_VIDEO] != NULL ?
			cur_sequence_tracks[MEDIA_TYPE_VIDEO] :
			cur_sequence_tracks[MEDIA_TYPE_AUDIO]);
		if (base_url->len != 0)
		{
			result_size += base_url->len + 1;
			if (track->file_info.uri.len > 0)
			{
				result_size += track->file_info.uri.len;
			}
			else
			{
				result_size += media_set->uri.len;
			}
		}
		result_size += conf->index_file_name_prefix.len;
		result_size += sizeof("-f-v-a") - 1 + VOD_INT32_LEN * 3;
		result_size += sizeof(m3u8_url_suffix) - 1;
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

	// write the streams
	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		cur_sequence_tracks = cur_sequence->filtered_clips[0].longest_track;

		// write the track information
		if (cur_sequence_tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			track = cur_sequence_tracks[MEDIA_TYPE_VIDEO];
			video = &track->media_info;
			bitrate = video->bitrate;
			if (cur_sequence_tracks[MEDIA_TYPE_AUDIO] != NULL)
			{
				audio = &cur_sequence_tracks[MEDIA_TYPE_AUDIO]->media_info;
				bitrate += audio->bitrate;
			}
			p = vod_sprintf(p, m3u8_stream_inf_video,
				bitrate,
				(uint32_t)video->u.video.width,
				(uint32_t)video->u.video.height,
				&video->codec_name);
			if (cur_sequence_tracks[MEDIA_TYPE_AUDIO] != NULL)
			{
				*p++ = ',';
				p = vod_copy(p, audio->codec_name.data, audio->codec_name.len);
			}
		}
		else
		{
			track = cur_sequence_tracks[MEDIA_TYPE_AUDIO];
			audio = &track->media_info;
			p = vod_sprintf(p, m3u8_stream_inf_audio, audio->bitrate, &audio->codec_name);
		}
		p = vod_copy(p, m3u8_stream_inf_suffix, sizeof(m3u8_stream_inf_suffix) - 1);

		// write the track url
		sequence_index = cur_sequence->index;
		if (base_url->len != 0)
		{
			// absolute url only
			p = vod_copy(p, base_url->data, base_url->len);
			if (track->file_info.uri.len != 0)
			{
				p = vod_copy(p, track->file_info.uri.data, track->file_info.uri.len);
				sequence_index = INVALID_SEQUENCE_INDEX;		// no need to pass the sequence index since we have a direct uri
			}
			else
			{
				p = vod_copy(p, media_set->uri.data, media_set->uri.len);
			}
			*p++ = '/';
		}

		p = vod_copy(p, conf->index_file_name_prefix.data, conf->index_file_name_prefix.len);
		if (media_set->has_multi_sequences && sequence_index != INVALID_SEQUENCE_INDEX)
		{
			p = vod_sprintf(p, "-f%uD", cur_sequence->index + 1);
		}

		if (cur_sequence_tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			p = vod_sprintf(p, "-v%uD", cur_sequence_tracks[MEDIA_TYPE_VIDEO]->index + 1);
		}

		if (cur_sequence_tracks[MEDIA_TYPE_AUDIO] != NULL)
		{
			p = vod_sprintf(p, "-a%uD", cur_sequence_tracks[MEDIA_TYPE_AUDIO]->index + 1);
		}

		p = vod_copy(p, m3u8_url_suffix, sizeof(m3u8_url_suffix) - 1);
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
	if (encryption_method == HLS_ENC_SAMPLE_AES)
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
