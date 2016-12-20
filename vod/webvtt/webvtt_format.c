#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include <ctype.h>

// constants
#define UTF8_BOM ("\xEF\xBB\xBF")
#define WEBVTT_HEADER ("WEBVTT")
#define WEBVTT_HEADER_NEWLINES ("WEBVTT\r\n\r\n")
#define WEBVTT_DURATION_ESTIMATE_CUES (10)
#define WEBVTT_CUE_MARKER ("-->")

// typedefs
typedef struct {
	size_t initial_read_size;
	size_t size_limit;
	bool_t first_time;
	vod_str_t buffer;
} webvtt_reader_state_t;

typedef struct {
	media_base_metadata_t base;
	vod_str_t source;
} webvtt_base_metadata_t;

static int64_t 
webvtt_read_timestamp(u_char* cur_pos, u_char** end_pos)
{
	int64_t hours;
	int64_t minutes;
	int64_t seconds;
	int64_t millis;

	// hour digits
	if (!isdigit(*cur_pos))
	{
		return -1;
	}

	hours = 0;
	for (; isdigit(*cur_pos); cur_pos++)
	{
		hours = hours * 10 + (*cur_pos - '0');
	}

	// colon
	if (*cur_pos != ':')
	{
		return -1;
	}
	cur_pos++;

	// 2 minute digits
	if (!isdigit(cur_pos[0]) || !isdigit(cur_pos[1]))
	{
		return -1;
	}
	minutes = (cur_pos[0] - '0') * 10 + (cur_pos[1] - '0');
	cur_pos += 2;

	// colon
	if (*cur_pos == ':')
	{
		cur_pos++;

		// 2 second digits
		if (!isdigit(cur_pos[0]) || !isdigit(cur_pos[1]))
		{
			return -1;
		}
		seconds = (cur_pos[0] - '0') * 10 + (cur_pos[1] - '0');
		cur_pos += 2;
	}
	else
	{
		// no hours
		seconds = minutes;
		minutes = hours;
		hours = 0;
	}

	// dot
	if (*cur_pos != '.' && *cur_pos != ',')
	{
		return -1;
	}
	cur_pos++;

	// 3 digit millis
	if (!isdigit(cur_pos[0]) || !isdigit(cur_pos[1]) || !isdigit(cur_pos[2]))
	{
		return -1;
	}
	millis = (cur_pos[0] - '0') * 100 + (cur_pos[1] - '0') * 10 + (cur_pos[2] - '0');

	if (end_pos != NULL)
	{
		*end_pos = cur_pos + 3;
	}

	return millis + 1000 * (seconds + 60 * (minutes + 60 * hours));
}

static bool_t
webvtt_identify_srt(u_char* p)
{
	// n digits
	if (!isdigit(*p))
	{
		return FALSE;
	}

	for (; isdigit(*p); p++);

	// new line
	switch (*p)
	{
	case '\r':
		p++;
		if (*p == '\n')
		{
			p++;
		}
		break;

	case '\n':
		p++;
		break;

	default:
		return FALSE;
	}

	// timestamp
	if (webvtt_read_timestamp(p, &p) < 0)
	{
		return FALSE;
	}

	for (; *p == ' ' || *p == '\t'; p++);

	// cue marker
	return vod_strncmp(p, WEBVTT_CUE_MARKER, sizeof(WEBVTT_CUE_MARKER) - 1) == 0;
}

static vod_status_t
webvtt_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t initial_read_size,
	size_t max_metadata_size,
	void** ctx)
{
	webvtt_reader_state_t* state;
	u_char* p = buffer->data;

	if (vod_strncmp(p, UTF8_BOM, sizeof(UTF8_BOM) - 1) == 0)
	{
		p += sizeof(UTF8_BOM) - 1;
	}

	if (buffer->len > 0 &&
		vod_strncmp(p, WEBVTT_HEADER, sizeof(WEBVTT_HEADER) - 1) != 0 && 
		!webvtt_identify_srt(p))
	{
		return VOD_NOT_FOUND;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_reader_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->first_time = TRUE;
	state->size_limit = 2 * 1024 * 1024;			// XXXXX support configuring different metadata size limits per format
	state->initial_read_size = initial_read_size;

	*ctx = state;
	return VOD_OK;
}

static vod_status_t
webvtt_reader_read(
	void* ctx,
	uint64_t offset,
	vod_str_t* buffer,
	media_format_read_metadata_result_t* result)
{
	webvtt_reader_state_t* state = ctx;

	if (buffer->len < state->initial_read_size || !state->first_time)
	{
		state->buffer = *buffer;
		result->parts = &state->buffer;
		result->part_count = 1;
		return VOD_OK;
	}

	// read up to the limit
	state->first_time = FALSE;
	result->read_req.flags = MEDIA_READ_FLAG_ALLOW_EMPTY_READ;
	result->read_req.read_offset = 0;
	result->read_req.read_size = state->size_limit;
	return VOD_AGAIN;
}

static u_char*
webvtt_find_next_cue(u_char* cur_pos)
{
	size_t dash_count = 0;

	for (; *cur_pos; cur_pos++)
	{
		switch (*cur_pos)
		{
		case '-':
			dash_count++;
			continue;
		
		case '>':
			if (dash_count >= 2)
			{
				return cur_pos + 1;
			}
			break;
		}

		dash_count = 0;
	}

	return NULL;
}

static u_char*
webvtt_find_prev_cue(u_char* cur_pos, u_char* limit)
{
	size_t match_count = 0;

	for (; cur_pos >= limit; cur_pos--)
	{
		switch (*cur_pos)
		{
		case '>':
			match_count = 1;
			break;

		case '-':
			if (match_count <= 0)
			{
				break;
			}

			match_count++;
			if (match_count >= sizeof(WEBVTT_CUE_MARKER) - 1)
			{
				return cur_pos;
			}
			break;

		default:
			match_count = 0;
			break;
		}
	}

	return NULL;
}

static u_char*
webvtt_find_prev_newline(u_char* cur_pos, u_char* limit)
{
	for (; cur_pos >= limit; cur_pos--)
	{
		if (*cur_pos == '\r' || *cur_pos == '\n')
		{
			return cur_pos;
		}
	}

	return NULL;
}

static u_char*
webvtt_find_prev_newline_no_limit(u_char* cur_pos)
{
	for (; ; cur_pos--)
	{
		if (*cur_pos == '\r' || *cur_pos == '\n')
		{
			return cur_pos;
		}
	}
}

static u_char*
webvtt_skip_newline_reverse_no_limit(u_char* cur_pos)
{
	if (*cur_pos == '\n' && cur_pos[-1] == '\r')
	{
		return cur_pos - 2;
	}

	return cur_pos - 1;
}

static u_char*
webvtt_find_next_empty_line(u_char* cur_pos, bool_t is_empty)
{
	for (; *cur_pos; cur_pos++)
	{
		switch (*cur_pos)
		{
		case '\r':
			if (cur_pos[1] == '\n')
			{
				cur_pos++;
			}
			break;

		case '\n':
			break;

		default:
			is_empty = FALSE;
			continue;
		}

		if (is_empty)
		{
			return cur_pos + 1;
		}
		is_empty = TRUE;
	}

	return NULL;
}

static uint64_t
webvtt_estimate_duration(vod_str_t* source)
{
	int64_t duration = 0;
	int64_t end_time;
	u_char* start_pos = source->data;
	u_char* cur_pos = start_pos + source->len;
	u_char* next_pos;
	int count;

	// Note: testing more than one cue since a previous cue may end after the last cue
	for (count = 0; count < WEBVTT_DURATION_ESTIMATE_CUES; count++)
	{
		// find next cue
		next_pos = webvtt_find_prev_cue(cur_pos, start_pos);
		if (next_pos == NULL)
		{
			break;
		}

		cur_pos = next_pos + sizeof(WEBVTT_CUE_MARKER) - 1;

		// parse end time
		for (; *cur_pos == ' ' || *cur_pos == '\t'; cur_pos++);

		end_time = webvtt_read_timestamp(cur_pos, NULL);
		if (end_time > duration)
		{
			duration = end_time;
		}

		cur_pos = next_pos;
	}

	return duration;
}

static vod_status_t
webvtt_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	webvtt_base_metadata_t* metadata;
	media_sequence_t* sequence;
	media_track_t* track;
	language_id_t lang_id;
	vod_str_t label;
	uint64_t full_duration;
	uint64_t duration;

	metadata = vod_alloc(request_context->pool, sizeof(*metadata));
	if (metadata == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	*result = &metadata->base;

	full_duration = webvtt_estimate_duration(source);

	if (!vod_codec_in_mask(VOD_CODEC_ID_WEBVTT, parse_params->codecs_mask) || 
		full_duration <= parse_params->clip_from)
	{
		metadata->base.tracks.nelts = 0;
		return VOD_OK;
	}

	// inherit the sequence language and label
	sequence = parse_params->source->sequence;
	if (sequence->label.len != 0)
	{
		label = sequence->label;
		lang_id = sequence->language;
	}
	else
	{
		// no language, assume English
		lang_id = VOD_LANG_EN;
		lang_get_native_name(lang_id, &label);
	}

	// filter by language
	if (parse_params->langs_mask != NULL &&
		!vod_is_bit_set(parse_params->langs_mask, lang_id))
	{
		metadata->base.tracks.nelts = 0;
		return VOD_OK;
	}

	if (vod_array_init(&metadata->base.tracks, request_context->pool, 1, sizeof(*track)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_parse: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	duration = vod_min(full_duration, parse_params->clip_to) - parse_params->clip_from;

	track = vod_array_push(&metadata->base.tracks);		// can't fail
	vod_memzero(track, sizeof(*track));
	track->media_info.media_type = MEDIA_TYPE_SUBTITLE;
	track->media_info.codec_id = VOD_CODEC_ID_WEBVTT;
	track->media_info.timescale = 1000;
	track->media_info.frames_timescale = 1000;
	track->media_info.duration = duration;
	track->media_info.full_duration = full_duration;
	track->media_info.duration_millis = duration;
	track->media_info.label = label;
	track->media_info.language = lang_id;
	track->media_info.bitrate = (source->len * 1000 * 8) / full_duration;

	metadata->source = *source;
	metadata->base.duration = duration;
	metadata->base.timescale = 1000;

	return VOD_OK;
}

static vod_status_t
webvtt_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	webvtt_base_metadata_t* metadata = vod_container_of(base, webvtt_base_metadata_t, base);
	media_track_t* track = base->tracks.elts;
	input_frame_t* cur_frame = NULL;
	vod_array_t frames;
	vod_str_t* source = &metadata->source;
	vod_str_t* header = &track->media_info.extra_data;
	vod_str_t cue_id = vod_null_string;
	uint64_t base_time;
	uint64_t clip_to;
	uint64_t start;
	uint64_t end;
	int64_t last_start_time = 0;
	int64_t start_time = 0;
	int64_t end_time = 0;
	u_char* timings_end;
	u_char* cur_pos = source->data;
	u_char* start_pos;
	u_char* cue_start;
	u_char* prev_line;
	u_char* p;

	// XXXXX consider adding a separate segmenter for subtitles

	vod_memzero(result, sizeof(*result));
	result->first_track = track;
	result->last_track = track + 1;
	result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
	result->total_track_count = 1;

	if ((parse_params->parse_type & (PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_EXTRA_DATA_SIZE)) == 0)
	{
		return VOD_OK;
	}

	// skip the file magic line
	if (vod_strncmp(cur_pos, UTF8_BOM, sizeof(UTF8_BOM) - 1) == 0)
	{
		cur_pos += sizeof(UTF8_BOM) - 1;
	}

	start_pos = cur_pos;
	header->data = cur_pos;

	if (vod_strncmp(cur_pos, WEBVTT_HEADER, sizeof(WEBVTT_HEADER) - 1) == 0)
	{
		cur_pos += sizeof(WEBVTT_HEADER) - 1;

		for (;;)
		{
			if (*cur_pos == '\r')
			{
				cur_pos++;
				if (*cur_pos == '\n')
				{
					cur_pos++;
				}
				break;
			}
			else if (*cur_pos == '\n')
			{
				cur_pos++;
				break;
			}
			else if (*cur_pos == '\0')
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"webvtt_parse_frames: eof while reading file magic line");
				return VOD_BAD_DATA;
			}

			cur_pos++;
		}

		// find the start of the first cue
		cur_pos = webvtt_find_next_cue(cur_pos);
		if (cur_pos == NULL)
		{
			header->len = source->len;
			header->data = vod_pstrdup(request_context->pool, header);
			if (header->data == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"webvtt_parse_frames: vod_pstrdup failed (1)");
				return VOD_ALLOC_FAILED;
			}
			return VOD_OK;
		}

		cur_pos = webvtt_find_prev_newline_no_limit(cur_pos);

		prev_line = webvtt_skip_newline_reverse_no_limit(cur_pos);
		if (*prev_line != '\r' && *prev_line != '\n')
		{
			cur_pos = webvtt_find_prev_newline(prev_line, start_pos);
			if (cur_pos == NULL)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"webvtt_parse_frames: failed to extract cue identifier");
				return VOD_BAD_DATA;
			}
		}

		cur_pos++;		// \r or \n
		header->len = cur_pos - header->data;
		header->data = vod_pstrdup(request_context->pool, header);
		if (header->data == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"webvtt_parse_frames: vod_pstrdup failed (2)");
			return VOD_ALLOC_FAILED;
		}
	}
	else
	{
		header->len = sizeof(WEBVTT_HEADER_NEWLINES) - 1;
		header->data = (u_char*)WEBVTT_HEADER_NEWLINES;
	}

	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_ALL) == 0)
	{
		return VOD_OK;
	}

	// cues
	if (vod_array_init(&frames, request_context->pool, 5, sizeof(*cur_frame)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_parse_frames: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	start = parse_params->range->start + parse_params->clip_from;

	if ((parse_params->parse_type & PARSE_FLAG_RELATIVE_TIMESTAMPS) != 0)
	{
		base_time = start;
		clip_to = parse_params->range->end - parse_params->range->start;
		end = clip_to;
	}
	else
	{
		base_time = parse_params->clip_from;
		clip_to = parse_params->clip_to;
		end = parse_params->range->end;		// Note: not adding clip_from, since end is checked after the clipping is applied to the timestamps
	}

	for (;;)
	{
		// find next cue
		cue_start = webvtt_find_next_cue(cur_pos);
		if (cue_start == NULL)
		{
			if (cur_frame != NULL)
			{
				cur_frame->duration = end_time - start_time;
				track->total_frames_duration = end_time - track->first_frame_time_offset;
			}
			break;
		}

		// parse end time
		cur_pos = cue_start;
		for (; *cur_pos == ' ' || *cur_pos == '\t'; cur_pos++);

		end_time = webvtt_read_timestamp(cur_pos, &timings_end);
		if (end_time < 0)
		{
			continue;
		}

		if ((uint64_t)end_time < start)
		{
			track->first_frame_index++;
			continue;
		}

		// start time
		cue_start = webvtt_find_prev_newline_no_limit(cue_start - (sizeof(WEBVTT_CUE_MARKER) - 1));

		start_time = webvtt_read_timestamp(cue_start + 1, NULL);
		if (start_time < 0 || start_time >= end_time)
		{
			continue;
		}

		// apply clipping
		if (start_time >= (int64_t)base_time)
		{
			start_time -= base_time;
			if ((uint64_t)start_time > clip_to)
			{
				start_time = clip_to;
			}
		}
		else
		{
			start_time = 0;
		}

		end_time -= base_time;
		if ((uint64_t)end_time > clip_to)
		{
			end_time = clip_to;
		}

		// adjust the duration of the previous frame
		if (cur_frame != NULL)
		{
			cur_frame->duration = start_time - last_start_time;
		}
		else
		{
			track->first_frame_time_offset = start_time;
		}

		if ((uint64_t)start_time >= end)
		{
			track->total_frames_duration = start_time - track->first_frame_time_offset;
			break;
		}

		// identifier
		prev_line = webvtt_skip_newline_reverse_no_limit(cue_start);
		if (*prev_line != '\r' && *prev_line != '\n')
		{
			cue_id.data = webvtt_find_prev_newline(prev_line, start_pos);
			if (cue_id.data == NULL)
			{
				cue_id.data = start_pos;
			}
			else
			{
				cue_id.data++;
			}
			cue_id.len = cue_start + 1 - cue_id.data;
		}
		else
		{
			cue_id.len = 0;
		}

		// find the end of the cue
		cur_pos = webvtt_find_next_empty_line(timings_end, FALSE);
		if (cur_pos == NULL)
		{
			cur_pos = source->data + source->len;
		}

		// allocate the frame
		cur_frame = vod_array_push(&frames);
		if (cur_frame == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"webvtt_parse_frames: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}

		// Note: mapping of cue into input_frame_t:
		//	- offset = pointer to buffer containing: cue id, cue settings list, cue payload
		//	- size = size of data pointed by offset
		//	- key_frame = cue id length
		//	- dts = start time
		//	- pts = end time

		cur_frame->pts_delay = end_time - start_time;
		cur_frame->size = cue_id.len + (cur_pos - timings_end);
		cur_frame->key_frame = cue_id.len;

		track->total_frames_size += cur_frame->size;

		p = vod_alloc(request_context->pool, cur_frame->size);
		if (p == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"webvtt_parse_frames: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}

		cur_frame->offset = (uintptr_t)p;

		p = vod_copy(p, cue_id.data, cue_id.len);
		vod_memcpy(p, timings_end, cur_pos - timings_end);

		last_start_time = start_time;
	}

	track->frame_count = frames.nelts;
	track->frames.first_frame = frames.elts;
	track->frames.last_frame = track->frames.first_frame + frames.nelts;

	return VOD_OK;
}

media_format_t webvtt_format = {
	FORMAT_ID_WEBVTT,
	vod_string("webvtt"),
	webvtt_reader_init,
	webvtt_reader_read,
	NULL,			// XXXXX consider implementing
	NULL,
	webvtt_parse,
	webvtt_parse_frames,
};
