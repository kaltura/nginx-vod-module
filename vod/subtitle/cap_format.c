#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include <ctype.h>

// macros
#define cap_is_style(ch) ((ch) < 0x20 || (ch) >= 0xc0)

// constants
#define CAP_DATA_START_OFFSET (0x80)
#define CAP_HEADER_SIZE_END_TIME (16)
#define CAP_HEADER_SIZE_NO_END_TIME (12)
#define CAP_TIMESTAMP_SIZE (4)
#define CAP_END_TIME_OFFSET (6)
#define CAP_START_TIME_OFFSET (2)
#define CAP_FLAG_HAS_END_TIME (0x20)

#define CAP_FRAME_RATE (30)
#define CAP_LAST_FRAME_DURATION (2000)

#define CAP_MIN_SPECIAL_CHAR (0x81)
#define CAP_MAX_SPECIAL_CHAR (0x95)

// globals
static const vod_str_t cap_special_chars[CAP_MAX_SPECIAL_CHAR - CAP_MIN_SPECIAL_CHAR + 1] = {
	vod_string("\xe2\x99\xaa"),
	vod_string("\xc3\xa1"),
	vod_string("\xc3\xa9"),
	vod_string("\xc3\xad"),
	vod_string("\xc3\xb3"),
	vod_string("\xc3\xba"),
	vod_string("\xc3\xa2"),
	vod_string("\xc3\xaa"),
	vod_string("\xc3\xae"),
	vod_string("\xc3\xb4"),
	vod_string("\xc3\xbb"),
	vod_string("\xc3\xa0"),
	vod_string("\xc3\xa8"),
	vod_string("\xc3\x91"),
	vod_string("\xc3\xb1"),
	vod_string("\xc3\xa7"),
	vod_string("\xc2\xa2"),
	vod_string("\xc2\xa3"),
	vod_string("\xc2\xbf"),
	vod_string("\xc2\xbd"),
	vod_string("\xc2\xae"),
};

static vod_status_t
cap_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t max_metadata_size,
	void** ctx)
{
	u_char* p = buffer->data;

	if (buffer->len < CAP_DATA_START_OFFSET ||
		p[0] != 0xea ||
		p[1] != 0x22 ||
		p[2] > 3)
	{
		return VOD_NOT_FOUND;
	}

	return subtitle_reader_init(
		request_context,
		ctx);
}

static uint64_t
cap_parse_timestamp(u_char* p, u_char hours_base)
{
	uint32_t millis = (1000 * p[3]) / CAP_FRAME_RATE;
	return ((uint64_t)p[0] - hours_base) * 3600000 + 	// hour
		((uint64_t)p[1]) * 60000 + 				// min
		((uint64_t)p[2]) * 1000 + 				// sec
		vod_min(millis, 999);
}

static uint64_t
cap_get_duration(vod_str_t* source)
{
	u_char* p = source->data + CAP_DATA_START_OFFSET;
	u_char* end = source->data + source->len;
	u_char hours_base = 0;
	uint64_t timestamp;
	uint64_t result = 0;
	uint32_t offset;
	uint32_t len;
	bool_t first_time = TRUE;

	while (p + 2 < end)
	{
		len = p[0];
		if (len <= 0)
		{
			p++;
			continue;
		}

		if (len > (uint32_t)(end - p))
		{
			break;
		}

		if ((p[1] & CAP_FLAG_HAS_END_TIME) != 0)
		{
			offset = CAP_END_TIME_OFFSET;
			timestamp = 0;
		}
		else
		{
			offset = CAP_START_TIME_OFFSET;
			timestamp = CAP_LAST_FRAME_DURATION;
		}

		if (offset + CAP_TIMESTAMP_SIZE > len)
		{
			p += len;
			continue;
		}

		if (first_time)
		{
			first_time = FALSE;
			hours_base = p[CAP_START_TIME_OFFSET];
		}

		timestamp += cap_parse_timestamp(p + offset, hours_base);
		if (timestamp > result)
		{
			result = timestamp;
		}

		p += len;
	}

	return result;
}

static vod_status_t
cap_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	return subtitle_parse(
		request_context,
		parse_params,
		source,
		NULL,
		cap_get_duration(source),
		metadata_part_count,
		result);
}

static u_char*
cap_get_next_block(u_char* p, u_char* end)
{
	uint32_t header_size;
	uint32_t len;

	while (p + 2 < end)
	{
		len = p[0];
		if (len <= 0)
		{
			p++;
			continue;
		}

		if (len > (uint32_t)(end - p))
		{
			break;
		}

		if ((p[1] & CAP_FLAG_HAS_END_TIME) != 0)
		{
			header_size = CAP_HEADER_SIZE_END_TIME;
		}
		else
		{
			header_size = CAP_HEADER_SIZE_NO_END_TIME;
		}

		if (header_size + 1 >= len)
		{
			p += len;
			continue;
		}

		return p;
	}

	return NULL;
}

static size_t
cap_get_max_text_len(u_char* p, u_char* end)
{
	size_t result = end - p + 3;		// 3 = \n\n\n
	u_char ch;

	for (; p < end; p++)
	{
		ch = *p;
		if (ch >= CAP_MIN_SPECIAL_CHAR && ch <= CAP_MAX_SPECIAL_CHAR)
		{
			result += cap_special_chars[ch - CAP_MIN_SPECIAL_CHAR].len - 1;
		}
		else if (ch != 0 && cap_is_style(ch))
		{
			result--;
		}
	}

	return result;
}

static u_char*
cap_parse_text(u_char* dest, u_char* src, u_char* src_end)
{
	const vod_str_t* special_char;
	u_char ch;

	*dest++ = '\n';

	for (; src < src_end; src++)
	{
		ch = *src;
		if (ch == 0)
		{
			if (dest[-1] != '\n')
			{
				*dest++ = '\n';
			}
		}
		else if (ch >= CAP_MIN_SPECIAL_CHAR && ch <= CAP_MAX_SPECIAL_CHAR)
		{
			special_char = &cap_special_chars[ch - CAP_MIN_SPECIAL_CHAR];
			dest = vod_copy(dest, special_char->data, special_char->len);
		}
		else if (!cap_is_style(ch))
		{
			*dest++ = ch;
		}
	}

	if (dest[-1] != '\n')
	{
		*dest++ = '\n';
	}
	*dest++ = '\n';

	return dest;
}

static vod_status_t
cap_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	subtitle_base_metadata_t* metadata = vod_container_of(base, subtitle_base_metadata_t, base);
	media_track_t* track = base->tracks.elts;
	input_frame_t* cur_frame = NULL;
	vod_array_t frames;
	vod_str_t* source = &metadata->source;
	vod_str_t* header = &track->media_info.extra_data;
	uint64_t last_start_time = 0;
	uint64_t start_time = 0;
	uint64_t end_time = 0;
	uint64_t base_time;
	uint64_t clip_to;
	uint64_t start;
	uint64_t end;
	u_char* cur_pos = source->data + CAP_DATA_START_OFFSET;
	u_char* end_pos = source->data + source->len;
	u_char* next;
	u_char* text_start;
	u_char* text_end;
	u_char* p;
	u_char hours_base = 0;
	bool_t first_time = TRUE;
	size_t frame_size;
	
	vod_memzero(result, sizeof(*result));
	result->first_track = track;
	result->last_track = track + 1;
	result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
	result->total_track_count = 1;

	header->len = sizeof(WEBVTT_HEADER_NEWLINES) - 1;
	header->data = (u_char*)WEBVTT_HEADER_NEWLINES;
	
	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_ALL) == 0)
	{
		return VOD_OK;
	}

	// cues
	if (vod_array_init(&frames, request_context->pool, 5, sizeof(*cur_frame)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"cap_parse_frames: vod_array_init failed");
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

	for (cur_pos = cap_get_next_block(cur_pos, end_pos); ; cur_pos = next)
	{
		if (cur_pos == NULL)
		{
			if (cur_frame != NULL)
			{
				cur_frame->duration = end_time - start_time;
				track->total_frames_duration = end_time - track->first_frame_time_offset;
			}
			break;
		}
		
		next = cap_get_next_block(cur_pos + cur_pos[0], end_pos);

		// get start / end
		if (first_time)
		{
			first_time = FALSE;
			hours_base = cur_pos[CAP_START_TIME_OFFSET];
		}

		start_time = cap_parse_timestamp(cur_pos + CAP_START_TIME_OFFSET, hours_base);
		if ((cur_pos[1] & CAP_FLAG_HAS_END_TIME) != 0)
		{
			end_time = cap_parse_timestamp(cur_pos + CAP_END_TIME_OFFSET, hours_base);
		}
		else
		{
			if (next != NULL)
			{
				end_time = cap_parse_timestamp(next + CAP_START_TIME_OFFSET, hours_base);
			}
			else
			{
				end_time = start_time + CAP_LAST_FRAME_DURATION;
			}
		}
		
		if (end_time < start)
		{
			track->first_frame_index++;
			continue;
		}
		
		if (start_time >= end_time)
		{
			continue;
		}
		
		// apply clipping
		if (start_time >= base_time)
		{
			start_time -= base_time;
			if (start_time > clip_to)
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

		// allocate the frame
		cur_frame = vod_array_push(&frames);
		if (cur_frame == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"cap_parse_frames: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}
		
		// allocate the text
		if ((cur_pos[1] & CAP_FLAG_HAS_END_TIME) != 0)
		{
			text_start = cur_pos + CAP_HEADER_SIZE_END_TIME;
		}
		else
		{
			text_start = cur_pos + CAP_HEADER_SIZE_NO_END_TIME;
		}
		text_end = cur_pos + cur_pos[0] - 1;

		frame_size = cap_get_max_text_len(text_start, text_end);

		p = vod_alloc(request_context->pool, frame_size);
		if (p == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"cap_parse_frames: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
		
		// Note: mapping of cue into input_frame_t:
		//	- offset = pointer to buffer containing: cue id, cue settings list, cue payload
		//	- size = size of data pointed by offset
		//	- key_frame = cue id length
		//	- dts = start time
		//	- pts = end time

		cur_frame->size = cap_parse_text(p, text_start, text_end) - p;
		if (cur_frame->size > frame_size)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"cap_parse_frames: result length %uD exceeded allocated length %uz",
				cur_frame->size, frame_size);
			return VOD_UNEXPECTED;
		}

		cur_frame->offset = (uintptr_t)p;
		cur_frame->pts_delay = end_time - start_time;
		cur_frame->key_frame = 0;
		track->total_frames_size += cur_frame->size;

		last_start_time = start_time;		
	}

	track->frame_count = frames.nelts;
	track->frames.first_frame = frames.elts;
	track->frames.last_frame = track->frames.first_frame + frames.nelts;

	return VOD_OK;
}

media_format_t cap_format = {
	FORMAT_ID_CAP,
	vod_string("cap"),
	cap_reader_init,
	subtitle_reader_read,
	NULL,
	NULL,
	cap_parse,
	cap_parse_frames,
};
