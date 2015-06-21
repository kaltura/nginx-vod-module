#include "m3u8_builder.h"
#include "hls_muxer.h"
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
static const char byte_range_tag_format[] = "#EXT-X-BYTERANGE:%uD@%uD\n";
static const u_char m3u8_url_suffix[] = ".m3u8\n";

// typedefs
typedef struct {
	u_char* p;
	vod_str_t required_tracks;
	vod_str_t* base_url;
	vod_str_t* segment_file_name_prefix;
} write_segment_context_t;

static int 
m3u8_builder_get_int_print_len(int n)
{
	int res = 1;
	while (n >= 10)
	{
		res++;
		n /= 10;
	}
	return res;
}

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
	bool_t include_file_index,
	mpeg_metadata_t* mpeg_metadata, 
	vod_str_t* required_tracks)
{
	mpeg_stream_metadata_t* cur_stream;
	uint32_t printed_file_indexes;
	uint32_t file_index;
	u_char* p;
	size_t result_size;

	result_size = mpeg_metadata->streams.nelts * (sizeof("-v") - 1 + m3u8_builder_get_int_print_len(mpeg_metadata->max_track_index + 1));
	if (include_file_index)
	{
		result_size += mpeg_metadata->streams.nelts * (sizeof("-f") - 1 + m3u8_builder_get_int_print_len(mpeg_metadata->first_stream->file_info.file_index + 1));
	}
	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_required_tracks_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	required_tracks->data = p;

	if (include_file_index)
	{
		printed_file_indexes = 0;
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			file_index = cur_stream->file_info.file_index;
			if ((printed_file_indexes & (1 << file_index)) != 0)
			{
				continue;
			}

			p = vod_sprintf(p, "-f%uD", file_index + 1);
			printed_file_indexes |= (1 << file_index);
		}
	}

	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		*p++ = '-';
		switch (cur_stream->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			*p++ = 'v';
			break;

		case MEDIA_TYPE_AUDIO:
			*p++ = 'a';
			break;

		default:
			continue;
		}

		p = vod_sprintf(p, "%uD", cur_stream->track_index + 1);
	}
	
	required_tracks->len = p - required_tracks->data;

	if (required_tracks->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_required_tracks_string: result length %uz exceeded allocated length %uz", 
			required_tracks->len, result_size);
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
	vod_str_t* required_tracks)
{
	p = vod_copy(p, base_url->data, base_url->len);
	p = vod_copy(p, segment_file_name_prefix->data, segment_file_name_prefix->len);
	*p++ = '-';
	p = vod_sprintf(p, "%uD", segment_index + 1);
	p = vod_copy(p, required_tracks->data, required_tracks->len);
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
		&ctx->required_tracks);
}

vod_status_t
m3u8_builder_build_iframe_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	bool_t include_file_index,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	write_segment_context_t ctx;
	size_t iframe_length;
	size_t result_size;
	hls_muxer_state_t muxer_state;
	bool_t simulation_supported;
	vod_status_t rc; 
	uint32_t segment_count;

	// initialize the muxer
	rc = hls_muxer_init(&muxer_state, request_context, 0, mpeg_metadata, NULL, NULL, NULL, &simulation_supported);
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
		include_file_index,
		mpeg_metadata, 
		&ctx.required_tracks);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// calculate the required buffer length
	segment_count = segmenter_conf->get_segment_count(segmenter_conf, mpeg_metadata->duration_millis);
	if (segment_count == INVALID_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_iframe_playlist: segment count is invalid");
		return VOD_BAD_DATA;
	}

	iframe_length = sizeof("#EXTINF:.000,\n") - 1 + m3u8_builder_get_int_print_len(vod_div_ceil(mpeg_metadata->duration_millis, 1000)) +
		sizeof(byte_range_tag_format) + VOD_INT32_LEN + m3u8_builder_get_int_print_len(MAX_FRAME_SIZE) - (sizeof("%uD%uD") - 1) +
		base_url->len + conf->segment_file_name_prefix.len + 1 + m3u8_builder_get_int_print_len(segment_count) + ctx.required_tracks.len + sizeof(".ts\n") - 1;

	result_size =
		conf->iframes_m3u8_header_len +
		iframe_length * mpeg_metadata->video_key_frame_count +
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

	if (mpeg_metadata->video_key_frame_count > 0)
	{
		ctx.base_url = base_url;
		ctx.segment_file_name_prefix = &conf->segment_file_name_prefix;
	
		rc = hls_muxer_simulate_get_iframes(&muxer_state, segmenter_conf, mpeg_metadata, m3u8_builder_append_iframe_string, &ctx);
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
	bool_t include_file_index,
	bool_t encryption_enabled,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	segment_durations_t segment_durations;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item;
	vod_str_t extinf;
	uint32_t segment_index;
	uint32_t last_segment_index;
	vod_str_t required_tracks;
	uint32_t scale;
	size_t segment_length;
	size_t result_size;
	vod_status_t rc;
	u_char* p;

	// build the required tracks string
	rc = m3u8_builder_build_required_tracks_string(
		request_context, 
		include_file_index,
		mpeg_metadata, 
		&required_tracks);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the segment durations
	rc = segmenter_conf->get_segment_durations(
		request_context,
		segmenter_conf,
		mpeg_metadata->longest_stream,
		MEDIA_TYPE_COUNT,
		&segment_durations);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the required buffer length
	segment_length = sizeof("#EXTINF:.000,\n") - 1 + m3u8_builder_get_int_print_len(vod_div_ceil(mpeg_metadata->duration_millis, 1000)) +
		segments_base_url->len + conf->segment_file_name_prefix.len + 1 + m3u8_builder_get_int_print_len(segment_durations.segment_count) + required_tracks.len + sizeof(".ts\n") - 1;

	result_size =
		sizeof(M3U8_HEADER_PART1) + VOD_INT64_LEN + 
		sizeof(M3U8_HEADER_PART2) + VOD_INT64_LEN + 
		segment_length * segment_durations.segment_count +
		sizeof(m3u8_footer);

	if (encryption_enabled)
	{
		result_size +=
			sizeof(encryption_key_tag_prefix) - 1 +
			base_url->len +
			conf->encryption_key_file_name.len + 
			sizeof("-f") - 1 + VOD_INT32_LEN + 
			sizeof(encryption_key_tag_postfix) - 1;
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
	
	if (encryption_enabled)
	{
		p = vod_copy(p, encryption_key_tag_prefix, sizeof(encryption_key_tag_prefix) - 1);
		p = vod_copy(p, base_url->data, base_url->len);
		p = vod_copy(p, conf->encryption_key_file_name.data, conf->encryption_key_file_name.len);
		if (include_file_index)
		{
			p = vod_sprintf(p, "-f%uD", mpeg_metadata->first_stream->file_info.file_index + 1);
		}
		p = vod_copy(p, encryption_key_tag_postfix, sizeof(encryption_key_tag_postfix)-1);
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

		// write the first segment
		extinf.data = p;
		p = m3u8_builder_append_extinf_tag(p, rescale_time(cur_item->duration, segment_durations.timescale, scale), scale);
		extinf.len = p - extinf.data;
		p = m3u8_builder_append_segment_name(p, segments_base_url, &conf->segment_file_name_prefix, segment_index, &required_tracks);
		segment_index++;

		// write any additional segments
		for (; segment_index < last_segment_index; segment_index++)
		{
			p = vod_copy(p, extinf.data, extinf.len);
			p = m3u8_builder_append_segment_name(p, segments_base_url, &conf->segment_file_name_prefix, segment_index, &required_tracks);
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
	bool_t include_file_index,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	WALK_STREAMS_BY_FILES_VARS(cur_file_streams);
	mpeg_stream_metadata_t* stream;
	media_info_t* video;
	media_info_t* audio = NULL;
	uint32_t bitrate;
	u_char* p;
	size_t max_video_stream_inf;
	size_t max_audio_stream_inf;
	size_t result_size;

	// calculate the result size
	max_video_stream_inf = 
		sizeof(m3u8_stream_inf_video) - 1 + 3 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
		MAX_CODEC_NAME_SIZE + 1 +
		sizeof(m3u8_stream_inf_suffix) - 1;
	max_audio_stream_inf = 
		sizeof(m3u8_stream_inf_audio) + VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
		sizeof(m3u8_stream_inf_suffix) - 1;
	result_size = 
		sizeof(m3u8_header) + 
		mpeg_metadata->stream_count[MEDIA_TYPE_VIDEO] * max_video_stream_inf +
		mpeg_metadata->stream_count[MEDIA_TYPE_AUDIO] * max_audio_stream_inf;

	WALK_STREAMS_BY_FILES_START(cur_file_streams, mpeg_metadata)

		stream = (cur_file_streams[MEDIA_TYPE_VIDEO] != NULL ?
			cur_file_streams[MEDIA_TYPE_VIDEO] :
			cur_file_streams[MEDIA_TYPE_AUDIO]);
		if (base_url->len != 0)
		{
			result_size += base_url->len;
			result_size += stream->file_info.uri.len + 1;
		}
		result_size += conf->index_file_name_prefix.len;
		result_size += sizeof("-f-v-a") - 1 + VOD_INT32_LEN * 3;
		result_size += sizeof(m3u8_url_suffix) - 1;
	
	WALK_STREAMS_BY_FILES_END(cur_file_streams, mpeg_metadata)

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
	WALK_STREAMS_BY_FILES_START(cur_file_streams, mpeg_metadata)

		// write the stream information
		if (cur_file_streams[MEDIA_TYPE_VIDEO] != NULL)
		{
			stream = cur_file_streams[MEDIA_TYPE_VIDEO];
			video = &stream->media_info;
			bitrate = video->bitrate;
			if (cur_file_streams[MEDIA_TYPE_AUDIO] != NULL)
			{
				audio = &cur_file_streams[MEDIA_TYPE_AUDIO]->media_info;
				bitrate += audio->bitrate;
			}
			p = vod_sprintf(p, m3u8_stream_inf_video, 
				bitrate, 
				(uint32_t)video->u.video.width,
				(uint32_t)video->u.video.height,
				&video->codec_name);
			if (cur_file_streams[MEDIA_TYPE_AUDIO] != NULL)
			{
				*p++ = ',';
				p = vod_copy(p, audio->codec_name.data, audio->codec_name.len);
			}
		}
		else
		{
			stream = cur_file_streams[MEDIA_TYPE_AUDIO];
			audio = &stream->media_info;
			p = vod_sprintf(p, m3u8_stream_inf_audio, audio->bitrate, &audio->codec_name);
		}
		p = vod_copy(p, m3u8_stream_inf_suffix, sizeof(m3u8_stream_inf_suffix) - 1);

		// write the stream url
		if (base_url->len != 0)
		{
			// absolute url only
			p = vod_copy(p, base_url->data, base_url->len);
			p = vod_copy(p, stream->file_info.uri.data, stream->file_info.uri.len);
			*p++ = '/';
		}

		p = vod_copy(p, conf->index_file_name_prefix.data, conf->index_file_name_prefix.len);
		if (base_url->len == 0 && include_file_index)
		{
			p = vod_sprintf(p, "-f%uD", stream->file_info.file_index + 1);
		}

		if (cur_file_streams[MEDIA_TYPE_VIDEO] != NULL)
		{
			p = vod_sprintf(p, "-v%uD", cur_file_streams[MEDIA_TYPE_VIDEO]->track_index + 1);
		}

		if (cur_file_streams[MEDIA_TYPE_AUDIO] != NULL)
		{
			p = vod_sprintf(p, "-a%uD", cur_file_streams[MEDIA_TYPE_AUDIO]->track_index + 1);
		}

		p = vod_copy(p, m3u8_url_suffix, sizeof(m3u8_url_suffix) - 1);

	WALK_STREAMS_BY_FILES_END(cur_file_streams, mpeg_metadata)

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
	uint32_t max_segment_duration)
{
	conf->m3u8_version = 3;

	conf->iframes_m3u8_header_len = vod_snprintf(
		conf->iframes_m3u8_header,
		sizeof(conf->iframes_m3u8_header) - 1,
		iframes_m3u8_header_format,
		vod_div_ceil(max_segment_duration, 1000)) - conf->iframes_m3u8_header;
}
