#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include <ctype.h>

// macros
#define webvtt_is_utf16le_bom(p) (p[0] == 0xff && p[1] == 0xfe)
#define webvtt_is_utf16be_bom(p) (p[0] == 0xfe && p[1] == 0xff)

// constants
#define WEBVTT_HEADER ("WEBVTT")
#define WEBVTT_DURATION_ESTIMATE_CUES (10)
#define WEBVTT_CUE_MARKER ("-->")

// utf8 functions
#define CHAR_TYPE u_char
#define CHAR_SIZE 1
#define METHOD(x) x
#include "webvtt_format_template.h"
#undef CHAR_TYPE
#undef CHAR_SIZE
#undef METHOD

#if (VOD_HAVE_ICONV)

#include <iconv.h>

// utf16 functions
#define CHAR_TYPE u_char
#define CHAR_SIZE 2
#define METHOD(x) x ## _utf16
#include "webvtt_format_template.h"
#undef CHAR_TYPE
#undef CHAR_SIZE
#undef METHOD

#define ICONV_INVALID_DESC ((iconv_t)-1)
#define ICONV_INITIAL_ALLOC_SIZE (100)
#define ICONV_SIZE_INCREMENT (20)

static iconv_t iconv_utf16le_to_utf8 = ICONV_INVALID_DESC;
static iconv_t iconv_utf16be_to_utf8 = ICONV_INVALID_DESC;

void
webvtt_init_process(vod_log_t* log)
{
	iconv_utf16le_to_utf8 = iconv_open("UTF-8", "UTF-16LE");
	if (iconv_utf16le_to_utf8 == ICONV_INVALID_DESC)
	{
		vod_log_error(VOD_LOG_WARN, log, vod_errno,
			"webvtt_init_process: iconv_open failed, utf16le srt is not supported");
	}

	iconv_utf16be_to_utf8 = iconv_open("UTF-8", "UTF-16BE");
	if (iconv_utf16be_to_utf8 == ICONV_INVALID_DESC)
	{
		vod_log_error(VOD_LOG_WARN, log, vod_errno,
			"webvtt_init_process: iconv_open failed, utf16be srt is not supported");
	}
}

void
webvtt_exit_process()
{
	if (iconv_utf16le_to_utf8 != ICONV_INVALID_DESC)
	{
		iconv_close(iconv_utf16le_to_utf8);
		iconv_utf16le_to_utf8 = ICONV_INVALID_DESC;
	}

	if (iconv_utf16be_to_utf8 != ICONV_INVALID_DESC)
	{
		iconv_close(iconv_utf16be_to_utf8);
		iconv_utf16be_to_utf8 = ICONV_INVALID_DESC;
	}
}

static vod_status_t
webvtt_utf16_to_utf8(
	request_context_t* request_context,
	iconv_t iconv_context,
	vod_str_t* input,
	vod_str_t* output)
{
	vod_array_t output_arr;
	vod_err_t err;
	u_char* end;
	size_t input_left;
	size_t output_left;
	char* input_pos;
	char* output_pos;

	// initialize the output array
	if (vod_array_init(
		&output_arr, 
		request_context->pool, 
		input->len / 2 + ICONV_INITIAL_ALLOC_SIZE, 
		1) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_utf16_to_utf8: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	input_pos = (char*)input->data;
	input_left = input->len;

	for (;;)
	{
		// process as much as possible
		output_pos = (char*)output_arr.elts + output_arr.nelts;
		output_left = output_arr.nalloc - output_arr.nelts;

		if (iconv(
			iconv_context,
			&input_pos, &input_left,
			&output_pos, &output_left) != (size_t)-1)
		{
			break;
		}

		err = vod_errno;
		if (err != E2BIG)
		{
			vod_log_error(VOD_LOG_WARN, request_context->log, err,
				"webvtt_utf16_to_utf8: iconv failed");
			return VOD_UNEXPECTED;
		}

		// grow the array
		output_arr.nelts = output_arr.nalloc - output_left;

		if (vod_array_push_n(&output_arr, ICONV_SIZE_INCREMENT) == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"webvtt_utf16_to_utf8: vod_array_push_n failed");
			return VOD_ALLOC_FAILED;
		}

		output_arr.nelts -= ICONV_SIZE_INCREMENT;
	}

	// null terminate
	output_arr.nelts = output_arr.nalloc - output_left;

	end = vod_array_push(&output_arr);
	if (end == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"webvtt_utf16_to_utf8: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}

	output_arr.nelts--;
	*end = '\0';

	// return the result
	output->data = output_arr.elts;
	output->len = output_arr.nelts;
	return VOD_OK;
}
#endif // VOD_HAVE_ICONV

static vod_status_t
webvtt_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t max_metadata_size,
	void** ctx)
{
	u_char* p = buffer->data;

#if (VOD_HAVE_ICONV)
	u_short* end;
	u_short last_char;
	bool_t result;

	if (webvtt_is_utf16le_bom(p) && buffer->len > 4)
	{
		if (iconv_utf16le_to_utf8 == ICONV_INVALID_DESC ||
			(buffer->len & 1) != 0)
		{
			return VOD_NOT_FOUND;
		}

		// make the buffer utf-16 null terminated
		// Note: it's ok to change the buffer since this function is never called on cache buffers
		end = (u_short*)(p + buffer->len) - 1;
		last_char = *end;
		*end = 0;

		// Note: the utf16 identification ignores the top byte of each char - it may get
		//		false positives, but the probability is very low. if this somehow happens,
		//		the file will be identified, but will fail to parse
		result = webvtt_identify_srt_utf16(p + 2);
		*end = last_char;

		if (!result)
		{
			return VOD_NOT_FOUND;
		}
	}
	else if (webvtt_is_utf16be_bom(p) && buffer->len > 4)
	{
		if (iconv_utf16be_to_utf8 == ICONV_INVALID_DESC ||
			(buffer->len & 1) != 0)
		{
			return VOD_NOT_FOUND;
		}

		// make the buffer utf-16 null terminated
		// Note: it's ok to change the buffer since this function is never called on cache buffers
		end = (u_short*)(p + buffer->len) - 1;
		last_char = *end;
		*end = 0;
		result = webvtt_identify_srt_utf16(p + 3);
		*end = last_char;

		if (!result)
		{
			return VOD_NOT_FOUND;
		}
	}
	else
#endif // VOD_HAVE_ICONV
	{
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
	}

	return subtitle_reader_init(
		request_context,
		ctx);
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
#if (VOD_HAVE_ICONV)
	u_char* p = source->data;
	vod_status_t rc;

	if (webvtt_is_utf16le_bom(p))
	{
		// skip the bom
		source->data += 2;
		source->len -= 2;

		// Note: the decoded buffer will be saved to cache, since source is changed to point to it
		rc = webvtt_utf16_to_utf8(request_context, iconv_utf16le_to_utf8, source, source);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	else if (webvtt_is_utf16be_bom(p))
	{
		// skip the bom
		source->data += 2;
		source->len -= 2;

		// Note: the decoded buffer will be saved to cache, since source is changed to point to it
		rc = webvtt_utf16_to_utf8(request_context, iconv_utf16be_to_utf8, source, source);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
#endif // VOD_HAVE_ICONV

	return subtitle_parse(
		request_context,
		parse_params,
		source,
		NULL,
		webvtt_estimate_duration(source),
		metadata_part_count,
		result);
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
	subtitle_base_metadata_t* metadata = vod_container_of(base, subtitle_base_metadata_t, base);
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
	subtitle_reader_read,
	NULL,			// XXXXX consider implementing
	NULL,
	webvtt_parse,
	webvtt_parse_frames,
};
