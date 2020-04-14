#include "ttml_builder.h"
#include "../mp4/mp4_fragment.h"
#include "../mp4/mp4_defs.h"

// constants
#define TTML_TIMESTAMP_FORMAT "%02uD:%02uD:%02uD.%03uD"
#define TTML_TIMESTAMP_MAX_SIZE (VOD_INT32_LEN + sizeof(":00:00.000") - 1)

#define TTML_HEADER										\
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"		\
	"<tt xmlns=\"http://www.w3.org/ns/ttml\">\n"		\
	"  <head/>\n"										\
	"  <body>\n"										\
	"    <div>\n"

#define TTML_FOOTER										\
	"    </div>\n"										\
	"  </body>\n"										\
	"</tt>\n"

#define TTML_P_HEADER_PART1 "      <p begin=\""
#define TTML_P_HEADER_PART2 "\" end=\""
#define TTML_P_HEADER_PART3 "\">"
#define TTML_P_FOOTER "</p>\n"

#define TTML_P_MAX_SIZE									\
	(sizeof(TTML_P_HEADER_PART1) - 1 +					\
	sizeof(TTML_P_HEADER_PART2) - 1 +					\
	sizeof(TTML_P_HEADER_PART3) - 1 +					\
	TTML_TIMESTAMP_MAX_SIZE * 2 +						\
	sizeof(TTML_P_FOOTER) - 1)

// typedefs
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char track_id[4];
	u_char default_sample_duration[4];
	u_char default_sample_size[4];
} ttml_tfhd_atom_t;

// globals
static u_char trun_atom[] = {
	0x00, 0x00, 0x00, 0x10, 0x74, 0x72, 0x75, 0x6e,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

static u_char sdtp_atom[] = {
	0x00, 0x00, 0x00, 0x0d,		// size
	0x73, 0x64, 0x74, 0x70,		// sdtp
	0x00, 0x00, 0x00, 0x00,		// version / flags
	0x2a						// sample_depends_on=2, sample_is_depended_on=2, sample_has_redundancy=2
};

static u_char*
ttml_write_tfhd_atom(u_char* p, uint32_t default_sample_duration, u_char** default_sample_size)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(ttml_tfhd_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'h', 'd');
	write_be32(p, 0x18);			// flags - default sample duration, default sample size
	write_be32(p, 1);				// track id
	write_be32(p, default_sample_duration);
	*default_sample_size = p;
	write_be32(p, 0);
	return p;
}

static u_char*
ttml_builder_write_timestamp(u_char* p, uint64_t timestamp)
{
	return vod_sprintf(p, TTML_TIMESTAMP_FORMAT,
		(uint32_t)(timestamp / 3600000),
		(uint32_t)((timestamp / 60000) % 60),
		(uint32_t)((timestamp / 1000) % 60),
		(uint32_t)(timestamp % 1000));
}

static u_char*
ttml_copy_payload_without_styles(
	u_char* p,
	u_char* src,
	uint32_t len)
{
	u_char* end = src + len;
	u_char* next_lt;

	// skip the cue settings (until first webvtt newline)
	for (; src < end; src++)
	{
		if (*src == '\r')
		{
			src++;
			if (*src == '\n')
			{
				src++;
			}
			break;
		}
		else if (*src == '\n')
		{
			src++;
			break;
		}
	}

	for (;;)
	{
		// copy up to next lt
		next_lt = memchr(src, '<', end - src);
		if (next_lt == NULL)
		{
			p = vod_copy(p, src, end - src);
			break;
		}

		p = vod_copy(p, src, next_lt - src);

		// skip up to next gt
		src = memchr(next_lt, '>', end - next_lt);
		if (src == NULL)
		{
			break;
		}
		src++;
	}

	return p;
}

size_t
ttml_builder_get_max_size(media_set_t* media_set)
{
	media_track_t* cur_track;
	size_t result;

	result =
		sizeof(TTML_HEADER) - 1 +
		sizeof(TTML_FOOTER) - 1;
	for (cur_track = media_set->filtered_tracks; cur_track < media_set->filtered_tracks_end; cur_track++)
	{
		result += cur_track->total_frames_size + TTML_P_MAX_SIZE * cur_track->frame_count;
	}

	return result;
}

u_char*
ttml_builder_write(media_set_t* media_set, u_char* p)
{
	frame_list_part_t* part;
	media_track_t* cur_track;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t start_time;
	uint32_t id_size;
	u_char* src;

	p = vod_copy(p, TTML_HEADER, sizeof(TTML_HEADER) - 1);

	for (cur_track = media_set->filtered_tracks; cur_track < media_set->filtered_tracks_end; cur_track++)
	{
		start_time = cur_track->clip_start_time + cur_track->first_frame_time_offset;
		part = &cur_track->frames;
		last_frame = part->last_frame;
		for (cur_frame = part->first_frame;; cur_frame++)
		{
			if (cur_frame >= last_frame)
			{
				if (part->next == NULL)
				{
					break;
				}
				part = part->next;
				cur_frame = part->first_frame;
				last_frame = part->last_frame;
			}

			// open p tag
			p = vod_copy(p, TTML_P_HEADER_PART1, sizeof(TTML_P_HEADER_PART1) - 1);
			p = ttml_builder_write_timestamp(p, start_time);
			p = vod_copy(p, TTML_P_HEADER_PART2, sizeof(TTML_P_HEADER_PART2) - 1);
			p = ttml_builder_write_timestamp(p, start_time + cur_frame->pts_delay);
			p = vod_copy(p, TTML_P_HEADER_PART3, sizeof(TTML_P_HEADER_PART3) - 1);
			start_time += cur_frame->duration;

			// cue body
			src = (u_char*)(uintptr_t)cur_frame->offset;
			id_size = cur_frame->key_frame;
			src += id_size;
			p = ttml_copy_payload_without_styles(p, src, cur_frame->size - id_size);

			// close p tag
			p = vod_copy(p, TTML_P_FOOTER, sizeof(TTML_P_FOOTER) - 1);
		}
	}

	p = vod_copy(p, TTML_FOOTER, sizeof(TTML_FOOTER) - 1);
	return p;
}

vod_status_t
ttml_build_mp4(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	uint32_t timescale,
	vod_str_t* result)
{
	size_t traf_atom_size;
	size_t moof_atom_size;
	size_t mdat_atom_size;
	size_t result_size;
	size_t ttml_size;
	u_char* default_sample_size;
	u_char* mdat_start;
	u_char* p;

	// get the result size
	ttml_size = ttml_builder_get_max_size(media_set);

	traf_atom_size = ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(ttml_tfhd_atom_t) +
		sizeof(trun_atom) +
		sizeof(sdtp_atom);

	moof_atom_size = ATOM_HEADER_SIZE + 
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t) + 
		traf_atom_size;

	result_size = moof_atom_size +
		ATOM_HEADER_SIZE +			// mdat
		ttml_size;

	// allocate the buffer
	p = vod_alloc(request_context->pool, result_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"ttml_build_mp4: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->data = p;

	// moof
	write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

	// moof.mfhd
	p = mp4_fragment_write_mfhd_atom(p, segment_index + 1);

	// moof.traf
	write_atom_header(p, traf_atom_size, 't', 'r', 'a', 'f');

	// moof.traf.tfhd
	p = ttml_write_tfhd_atom(p, 
		rescale_time(media_set->segment_duration, 1000, timescale), 
		&default_sample_size);

	// moof.traf.trun
	p = vod_copy(p, trun_atom, sizeof(trun_atom));

	// moof.traf.sdtp
	p = vod_copy(p, sdtp_atom, sizeof(sdtp_atom));

	// mdat
	mdat_start = p;
	write_atom_header(p, 0, 'm', 'd', 'a', 't');
	p = ttml_builder_write(media_set, p);

	// update sizes
	mdat_atom_size = p - mdat_start;
	write_be32(mdat_start, mdat_atom_size);
	mdat_atom_size -= ATOM_HEADER_SIZE;
	write_be32(default_sample_size, mdat_atom_size);

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"ttml_build_mp4: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}
