#include "m3u8_builder.h"
#include "common.h"
#include "muxer.h"

// macros
#define MAX_SEGMENT_COUNT (10 * 1024)			// more than 1 day when using 10 sec segments

// constants
static const u_char m3u8_footer[] = "#EXT-X-ENDLIST\n";
static const char byte_range_tag_format[] = "#EXT-X-BYTERANGE:%uD@%uD\n";

// typedefs
typedef struct {
	u_char* p;
	vod_str_t required_tracks;
	vod_str_t* segment_file_name_prefix;
} append_iframe_context_t;

static int 
get_int_print_len(int n)
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
//	1. not using vod_sprintf since it doesn't have the option to limit the precision
//		without forcing it (e.g. have 1.5 printed as 1.5 and 1.12345 printed as 1.123)
//  2. scale must be a power of 10

static u_char* 
format_double(u_char* p, uint32_t n, uint32_t scale)
{
	int cur_digit;
	int int_n = n / scale;
	int fraction = n % scale;

	p = vod_sprintf(p, "%d", int_n);

	if (fraction == 0)
		return p;

	*p++ = '.';
	while (fraction)
	{
		scale /= 10;
		cur_digit = fraction / scale;
		*p++ = cur_digit + '0';
		fraction -= cur_digit * scale;
	}
	return p;
}

static vod_status_t
build_required_tracks_string(request_context_t* request_context, mpeg_metadata_t* mpeg_metadata, uint32_t max_track_index, vod_str_t* required_tracks)
{
	mpeg_stream_metadata_t* cur_stream;
	mpeg_stream_metadata_t* streams_end;
	u_char* buffer;

	required_tracks->len = mpeg_metadata->streams.nelts * (sizeof("-v") - 1 + get_int_print_len(max_track_index + 1));
	buffer = vod_alloc(request_context->pool, required_tracks->len + 1);
	if (buffer == NULL)
	{
		return VOD_ALLOC_FAILED;
	}
	required_tracks->data = buffer;

	cur_stream = (mpeg_stream_metadata_t*)mpeg_metadata->streams.elts;
	streams_end = cur_stream + mpeg_metadata->streams.nelts;
	for (; cur_stream < streams_end; cur_stream++)
	{
		*buffer++ = '-';
		switch (cur_stream->media_type)
		{
		case MEDIA_TYPE_VIDEO:
			*buffer++ = 'v';
			break;

		case MEDIA_TYPE_AUDIO:
			*buffer++ = 'a';
			break;

		default:
			continue;
		}

		buffer = vod_sprintf(buffer, "%d", cur_stream->track_index + 1);
	}
	required_tracks->len = buffer - required_tracks->data;

	return VOD_OK;
}

static u_char*
append_segment_name(u_char* p, vod_str_t* segment_file_name_prefix, uint32_t segment_index, vod_str_t* required_tracks)
{
	p = vod_copy(p, segment_file_name_prefix->data, segment_file_name_prefix->len);
	p = vod_sprintf(p, "%d", segment_index);
	p = vod_copy(p, required_tracks->data, required_tracks->len);
	p = vod_copy(p, ".ts\n", sizeof(".ts\n") - 1);
	return p;
}

u_char*
append_extinf_tag(u_char* p, uint32_t duration, uint32_t scale)
{
	p = vod_copy(p, "#EXTINF:", sizeof("#EXTINF:") - 1);
	p = format_double(p, duration, scale);
	*p++ = ',';
	*p++ = '\n';
	return p;
}

static void
append_iframe_string(void* context, int segment_index, uint32_t frame_duration, uint32_t frame_start, uint32_t frame_size)
{
	append_iframe_context_t* ctx = (append_iframe_context_t*)context;

	ctx->p = append_extinf_tag(ctx->p, frame_duration, 1000);
	ctx->p = vod_sprintf(ctx->p, byte_range_tag_format, frame_size, frame_start);
	ctx->p = append_segment_name(ctx->p, ctx->segment_file_name_prefix, segment_index, &ctx->required_tracks);
}

vod_status_t
build_iframe_playlist_m3u8(
	request_context_t* request_context,
	m3u8_config_t* conf,
	uint32_t segment_duration,
	mpeg_metadata_t* mpeg_metadata, 
	vod_str_t* result)
{
	append_iframe_context_t append_iframe_context;
	mpeg_stream_metadata_t* cur_stream;
	mpeg_stream_metadata_t* streams_end;
	int64_t duration = 0;
	uint32_t duration_millis;
	uint32_t segment_count;
	uint32_t iframe_length;
	uint32_t total_size;
	uint32_t key_frame_count = 0;
	muxer_state_t muxer_state;
	bool_t simulation_supported;
	uint32_t max_track_index = 0;
	vod_status_t rc;

	// initialize the muxer
	rc = muxer_init(&muxer_state, request_context, 0, mpeg_metadata, NULL, NULL, NULL, &simulation_supported);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (!simulation_supported)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"build_iframe_playlist_m3u8: simulation not supported for this file, cant create iframe playlist");
		return VOD_BAD_REQUEST;
	}

	// get the max duration and track index
	cur_stream = (mpeg_stream_metadata_t*)mpeg_metadata->streams.elts;
	streams_end = cur_stream + mpeg_metadata->streams.nelts;
	for (; cur_stream < streams_end; cur_stream++)
	{
		duration = MAX(duration, cur_stream->duration);
		max_track_index = MAX(max_track_index, cur_stream->track_index);
		if (cur_stream->media_type == MEDIA_TYPE_VIDEO)
		{
			key_frame_count += cur_stream->key_frame_count;
		}
	}

	// build the required tracks string
	rc = build_required_tracks_string(request_context, mpeg_metadata, max_track_index, &append_iframe_context.required_tracks);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// calculate the required buffer length
	duration_millis = DIV_CEIL(duration, 90);

	segment_count = DIV_CEIL(duration_millis, segment_duration);

	iframe_length = sizeof("#EXTINF:.00000,\n") - 1 + get_int_print_len(DIV_CEIL(duration_millis, 1000)) +
		sizeof(byte_range_tag_format) + VOD_INT32_LEN + get_int_print_len(MAX_FRAME_SIZE) - (sizeof("%uD%uD") - 1) +
		conf->segment_file_name_prefix.len + get_int_print_len(segment_count) + append_iframe_context.required_tracks.len + sizeof(".ts\n") - 1;

	total_size =
		conf->iframes_m3u8_header_len +
		iframe_length * key_frame_count +
		sizeof(m3u8_footer);

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, total_size);
	if (result->data == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	// fill out the buffer
	append_iframe_context.p = vod_copy(result->data, conf->iframes_m3u8_header, conf->iframes_m3u8_header_len);
	append_iframe_context.segment_file_name_prefix = &conf->segment_file_name_prefix;

	muxer_simulate_get_iframes(&muxer_state, segment_duration, append_iframe_string, &append_iframe_context);

	result->len = vod_copy(append_iframe_context.p, m3u8_footer, sizeof(m3u8_footer)-1) - result->data;

	return VOD_OK;
}

vod_status_t
build_index_playlist_m3u8(
	request_context_t* request_context,
	m3u8_config_t* conf,
	uint32_t segment_duration,
	uint32_t clip_to,
	uint32_t clip_from,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	mpeg_stream_metadata_t* cur_stream;
	mpeg_stream_metadata_t* streams_end;
	vod_str_t required_tracks;
	uint32_t segment_count;
	uint32_t segment_index;
	uint32_t total_size;
	uint32_t max_track_index = 0;
	uint32_t duration_millis;
	int64_t duration = 0;
	u_char* p;
	vod_status_t rc;

	// get max duration and track index
	cur_stream = (mpeg_stream_metadata_t*)mpeg_metadata->streams.elts;
	streams_end = cur_stream + mpeg_metadata->streams.nelts;
	for (; cur_stream < streams_end; cur_stream++)
	{
		duration = MAX(duration, cur_stream->duration);
		max_track_index = MAX(max_track_index, cur_stream->track_index);
	}

	// get the effective duration
	duration_millis = DIV_CEIL(duration, 90);

	if (duration_millis <= clip_from)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"build_index_playlist_m3u8: clip from %uD exceeds the file duration %uD", clip_from, duration_millis);
		return VOD_BAD_REQUEST;
	}

	duration_millis = MIN(duration_millis - clip_from, clip_to);

	// build the required tracks string
	rc = build_required_tracks_string(request_context, mpeg_metadata, max_track_index, &required_tracks);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the required buffer length
	segment_count = DIV_CEIL(duration_millis, segment_duration);
	if (segment_count > MAX_SEGMENT_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"build_index_playlist_m3u8: invalid segment count %uD", segment_count);
		return VOD_BAD_DATA;
	}

	total_size =
		conf->m3u8_header_len +
		(conf->m3u8_extinf_len + 
		conf->segment_file_name_prefix.len + get_int_print_len(segment_count) + required_tracks.len + sizeof(".ts\n") - 1) * segment_count +
		sizeof(m3u8_footer);

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, total_size);
	if (result->data == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	// fill out the buffer
	p = vod_copy(result->data, conf->m3u8_header, conf->m3u8_header_len);	//sizeof(m3u8_header)-1);
	for (segment_index = 1; duration_millis > 0; segment_index++)
	{
		if (duration_millis >= segment_duration)
		{
			p = vod_copy(p, conf->m3u8_extinf, conf->m3u8_extinf_len);
			duration_millis -= segment_duration;
		}
		else
		{
			if (conf->m3u8_version >= 3)
			{
				p = append_extinf_tag(p, duration_millis, 1000);
			}
			else
			{
				p = append_extinf_tag(p, (duration_millis + 500) / 1000, 1);
			}
			duration_millis = 0;
		}

		p = append_segment_name(p, &conf->segment_file_name_prefix, segment_index, &required_tracks);
	}

	p = vod_copy(p, m3u8_footer, sizeof(m3u8_footer)-1);

	result->len = p - result->data;

	return VOD_OK;
}

void 
init_m3u8_config(
	m3u8_config_t* conf, 
	uint32_t segment_duration, 
	const char* encryption_key_file_name)
{
	conf->m3u8_version = (segment_duration % 1000 == 0) ? 2 : 3;

	conf->m3u8_extinf_len = append_extinf_tag(
		conf->m3u8_extinf,
		segment_duration,
		1000) - conf->m3u8_extinf;

	conf->m3u8_header_len = vod_snprintf(
		conf->m3u8_header,
		sizeof(conf->m3u8_header) - 1,
		m3u8_header_format,
		(segment_duration + 500) / 1000,		// EXT-X-TARGETDURATION should be the segment duration rounded to nearest second
		encryption_key_file_name ? encryption_key_tag_prefix : "",
		encryption_key_file_name ? encryption_key_file_name : "",
		encryption_key_file_name ? encryption_key_tag_postfix : "",
		conf->m3u8_version) - conf->m3u8_header;

	conf->iframes_m3u8_header_len = vod_snprintf(
		conf->iframes_m3u8_header,
		sizeof(conf->iframes_m3u8_header) - 1,
		iframes_m3u8_header_format,
		DIV_CEIL(segment_duration, 1000)) - conf->iframes_m3u8_header;
}
