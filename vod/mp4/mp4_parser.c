#include <limits.h>
#include "mp4_parser.h"
#include "mp4_defs.h"
#include "../read_stream.h"
#include "../codec_config.h"
#include "../common.h"

// TODO: use iterators from mp4_parser_base.c to reduce code duplication

// macros
#define member_size(type, member) sizeof(((type *)0)->member)

#define set_raw_atom(target, source)							\
	{															\
	(target).ptr = (source).ptr - (source).header_size;		\
	(target).size = (source).size + (source).header_size;	\
	}

// constants
#define MAX_FRAMERATE_TEST_SAMPLES (20)
#define MAX_TOTAL_SIZE_TEST_SAMPLES (100000)
#define MAX_STREAM_COUNT (1024)
#define MAX_DURATION_SEC (1000000)

// trak atom parsing
typedef struct {
	atom_info_t stco;
	atom_info_t stsc;
	atom_info_t stsz;
	atom_info_t stts;
	atom_info_t ctts;
	atom_info_t stss;
	atom_info_t stsd;
	atom_info_t hdlr;
	atom_info_t mdhd;
	atom_info_t dinf;
	atom_info_t tkhd;
} trak_atom_infos_t;

typedef struct {
	request_context_t* request_context;
	mpeg_parse_params_t parse_params;
	uint32_t track_indexes[MEDIA_TYPE_COUNT];
	file_info_t* file_info;
	mpeg_base_metadata_t* result;
} process_moov_context_t;

typedef struct {
	request_context_t* request_context;
	media_info_t media_info;
	mpeg_parse_params_t parse_params;
} metadata_parse_context_t;

typedef struct {
	// input
	request_context_t* request_context;
	media_info_t* media_info;
	mpeg_parse_params_t parse_params;
	const uint32_t* stss_start_pos;			// initialized only when aligning keyframes
	uint32_t stss_entries;					// initialized only when aligning keyframes

	// output
	uint32_t stss_start_index;
	uint32_t dts_shift;
	uint32_t first_frame;
	uint32_t last_frame;
	uint64_t first_frame_time_offset;
	int32_t clip_from_frame_offset;
	input_frame_t* frames;
	uint64_t* frame_offsets;
	uint32_t frame_count;
	uint64_t total_frames_size;
	uint64_t total_frames_duration;
	uint32_t key_frame_count;
	uint32_t first_chunk_frame_index;
	bool_t chunk_equals_sample;
	uint64_t first_frame_chunk_offset;
} frames_parse_context_t;

typedef struct {
	trak_atom_infos_t trak_atom_infos;
	media_info_t media_info;
	file_info_t file_info;
	uint32_t track_index;
} mpeg_stream_base_metadata_t;

typedef struct {
	vod_status_t(*parse)(atom_info_t* atom_info, frames_parse_context_t* context);
	int offset;
	uint32_t flag;
} trak_atom_parser_t;

static const relevant_atom_t relevant_atoms_stbl[] = {
	{ ATOM_NAME_STCO, offsetof(trak_atom_infos_t, stco), NULL },
	{ ATOM_NAME_CO64, offsetof(trak_atom_infos_t, stco), NULL },
	{ ATOM_NAME_STSC, offsetof(trak_atom_infos_t, stsc), NULL },
	{ ATOM_NAME_STSZ, offsetof(trak_atom_infos_t, stsz), NULL },
	{ ATOM_NAME_STZ2, offsetof(trak_atom_infos_t, stsz), NULL },
	{ ATOM_NAME_STTS, offsetof(trak_atom_infos_t, stts), NULL },
	{ ATOM_NAME_CTTS, offsetof(trak_atom_infos_t, ctts), NULL },
	{ ATOM_NAME_STSS, offsetof(trak_atom_infos_t, stss), NULL },
	{ ATOM_NAME_STSD, offsetof(trak_atom_infos_t, stsd), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_minf[] = {
	{ ATOM_NAME_STBL, 0, relevant_atoms_stbl },
	{ ATOM_NAME_DINF, offsetof(trak_atom_infos_t, dinf), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_mdia[] = {
	{ ATOM_NAME_MINF, 0, relevant_atoms_minf },
	{ ATOM_NAME_HDLR, offsetof(trak_atom_infos_t, hdlr), NULL },
	{ ATOM_NAME_MDHD, offsetof(trak_atom_infos_t, mdhd), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};	

static const relevant_atom_t relevant_atoms_trak[] = {
	{ ATOM_NAME_MDIA, 0, relevant_atoms_mdia },
	{ ATOM_NAME_TKHD, offsetof(trak_atom_infos_t, tkhd), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

typedef struct {
	int raw_atom_index;
	int atom_info_offset;
} raw_atom_mapping_t;

const raw_atom_mapping_t raw_atom_mapping[] = {
	{ RTA_TKHD, offsetof(trak_atom_infos_t, tkhd) },
	{ RTA_HDLR, offsetof(trak_atom_infos_t, hdlr) },
	{ RTA_MDHD, offsetof(trak_atom_infos_t, mdhd) },
	{ RTA_DINF, offsetof(trak_atom_infos_t, dinf) },
	{ RTA_STSD, offsetof(trak_atom_infos_t, stsd) },
};

// implementation
static vod_status_t 
mp4_parser_find_atom_callback(void* ctx, atom_info_t* atom_info)
{
	atom_info_t* context = (atom_info_t*)ctx;
	
	if (atom_info->name != context->name)
	{
		context->ptr = atom_info->ptr + atom_info->size;
		return VOD_OK;
	}
	
	*context = *atom_info;
	
	return VOD_BAD_DATA;		// just to quit the loop, not really an error
}

static vod_status_t 
mp4_parser_parse_hdlr_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const hdlr_atom_t* atom = (const hdlr_atom_t*)atom_info->ptr;
	uint32_t type;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_hdlr_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}
		
	type = parse_le32(atom->type);
	switch (type)
	{
	case HDLR_TYPE_VIDE:
		context->media_info.media_type = MEDIA_TYPE_VIDEO;
		break;

	case HDLR_TYPE_SOUN:
		context->media_info.media_type = MEDIA_TYPE_AUDIO;
		break;

	default:
		context->media_info.media_type = MEDIA_TYPE_NONE;
		break;
	}
	
	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_tkhd_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const tkhd_atom_t* atom = (const tkhd_atom_t*)atom_info->ptr;
	const tkhd64_atom_t* atom64 = (const tkhd64_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_tkhd_atom: atom size %uL too small (1)", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_tkhd_atom: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}

		context->media_info.track_id = parse_be32(atom64->track_id);
	}
	else
	{
		context->media_info.track_id = parse_be32(atom->track_id);
	}

	if (context->media_info.track_id == 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_tkhd_atom: invalid track id");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_mdhd_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const mdhd_atom_t* atom = (const mdhd_atom_t*)atom_info->ptr;
	const mdhd64_atom_t* atom64 = (const mdhd64_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_mdhd_atom: atom size %uL too small (1)", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_mdhd_atom: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}
			
		context->media_info.timescale = parse_be32(atom64->timescale) * context->parse_params.speed_nom;
		context->media_info.duration = parse_be64(atom64->duration) * context->parse_params.speed_denom;
	}
	else
	{
		context->media_info.timescale = parse_be32(atom->timescale) * context->parse_params.speed_nom;
		context->media_info.duration = ((uint64_t)parse_be32(atom->duration)) * context->parse_params.speed_denom;
	}
	
	if (context->media_info.timescale == 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_mdhd_atom: timescale is zero");
		return VOD_BAD_DATA;
	}

	if (context->media_info.duration > (uint64_t)MAX_DURATION_SEC * context->media_info.timescale)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_mdhd_atom: media duration %uL too big", context->media_info.duration);
		return VOD_BAD_DATA;
	}

	context->media_info.duration_millis = rescale_time(context->media_info.duration, context->media_info.timescale, 1000);

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_stts_atom_frame_duration_only(atom_info_t* atom_info, frames_parse_context_t* context)
{
	media_info_t* media_info = context->media_info;
	const stts_entry_t* last_entry;
	const stts_entry_t* cur_entry;
	uint32_t entries;
	uint32_t cur_duration;
	vod_status_t rc;

	rc = mp4_parser_validate_stts_data(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (entries > MAX_FRAMERATE_TEST_SAMPLES)
	{
		entries = MAX_FRAMERATE_TEST_SAMPLES;			// take only a some of the samples
	}

	cur_entry = (const stts_entry_t*)(atom_info->ptr + sizeof(stts_atom_t));
	last_entry = cur_entry + entries;

	for (; cur_entry < last_entry; cur_entry++)
	{
		cur_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
		if (cur_duration == 0)
		{
			continue;
		}

		if (cur_duration != 0 && (media_info->min_frame_duration == 0 || cur_duration < media_info->min_frame_duration))
		{
			media_info->min_frame_duration = cur_duration;
		}

		if (cur_duration > media_info->max_frame_duration)
		{
			media_info->max_frame_duration = cur_duration;
		}
	}

	if (media_info->min_frame_duration == 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stts_atom_frame_duration_only: min frame duration is zero");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_stts_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	uint32_t timescale = context->media_info->timescale;
	const stts_entry_t* last_entry;
	const stts_entry_t* cur_entry;
	uint32_t sample_count;
	uint32_t sample_duration;
	uint32_t entries;
	uint64_t clip_from;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t clip_from_accum_duration = 0;
	uint64_t accum_duration = 0;
	uint64_t next_accum_duration;
	uint32_t cur_count;
	uint32_t skip_count;
	uint32_t initial_alloc_size;
	input_frame_t* cur_frame_limit;
	input_frame_t* cur_frame;
	vod_array_t frames_array;
	uint32_t first_frame;
	uint32_t frame_index = 0;
	uint32_t key_frame_index;
	uint32_t key_frame_stss_index;
	const uint32_t* stss_entry;
	vod_status_t rc;

	// validate the atom
	rc = mp4_parser_validate_stts_data(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	// parse the first sample
	cur_entry = (const stts_entry_t*)(atom_info->ptr + sizeof(stts_atom_t));
	last_entry = cur_entry + entries;
	if (cur_entry >= last_entry)
	{
		if (context->stss_entries != 0)
		{
			context->request_context->start = 0;
			context->request_context->end = 0;
		}
		return VOD_OK;
	}

	sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
	sample_count = parse_be32(cur_entry->count);
	next_accum_duration = accum_duration + sample_duration * sample_count;

	if (context->parse_params.clip_from > 0)
	{
		clip_from = (((uint64_t)context->parse_params.clip_from * timescale) / 1000);

		for (;;)
		{
			if (sample_duration > 0 &&
				clip_from + sample_duration - 1 < next_accum_duration)
			{
				break;
			}

			frame_index += sample_count;
			accum_duration = next_accum_duration;

			// parse the next sample
			cur_entry++;
			if (cur_entry >= last_entry)
			{
				if (context->stss_entries != 0)
				{
					context->request_context->start = 0;
					context->request_context->end = 0;
				}
				return VOD_OK;
			}

			sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
			sample_count = parse_be32(cur_entry->count);
			next_accum_duration = accum_duration + sample_duration * sample_count;
		}

		// calculate the clip from duration
		skip_count = vod_div_ceil(clip_from - accum_duration, sample_duration);
		clip_from_accum_duration = accum_duration + skip_count * sample_duration;

		context->clip_from_frame_offset = clip_from_accum_duration - clip_from;
	}

	// skip to the sample containing the start time
	start_time = (((uint64_t)context->request_context->start * timescale) / context->request_context->timescale);

	for (;;)
	{
		if (sample_duration > 0 && 
			start_time + sample_duration - 1 < next_accum_duration)
		{
			break;
		}

		frame_index += sample_count;
		accum_duration = next_accum_duration;

		// parse the next sample
		cur_entry++;
		if (cur_entry >= last_entry)
		{
			if (context->stss_entries != 0)
			{
				if (context->media_info->duration > clip_from_accum_duration)
				{
					context->first_frame_time_offset = context->media_info->duration - clip_from_accum_duration;
				}
				context->request_context->start = 0;
				context->request_context->end = 0;
			}
			return VOD_OK;
		}

		sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
		sample_count = parse_be32(cur_entry->count);
		next_accum_duration = accum_duration + sample_duration * sample_count;
	}

	// skip to the start time within the current entry
	if (start_time > accum_duration)
	{
		skip_count = vod_div_ceil(start_time - accum_duration, sample_duration);
		sample_count -= skip_count;
		frame_index += skip_count;
		accum_duration += skip_count * sample_duration;
	}

	if (context->stss_entries != 0)
	{
		// jump to the first key frame after the start position
		context->stss_start_index = mp4_parser_find_stss_entry(frame_index, context->stss_start_pos, context->stss_entries);
		if (context->stss_start_index >= context->stss_entries)
		{
			// can't find any key frame after the start pos
			if (context->media_info->duration > clip_from_accum_duration)
			{
				context->first_frame_time_offset = context->media_info->duration - clip_from_accum_duration;
			}
			context->request_context->start = 0;
			context->request_context->end = 0;
			return VOD_OK;
		}

		stss_entry = context->stss_start_pos + context->stss_start_index;
		key_frame_index = parse_be32(stss_entry) - 1;

		// skip to the sample containing the key frame
		while (key_frame_index >= frame_index + sample_count)
		{
			frame_index += sample_count;
			accum_duration = next_accum_duration;

			// fetch next sample duration and count
			cur_entry++;
			if (cur_entry >= last_entry)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: atom ended before the first key frame");
				return VOD_BAD_DATA;
			}

			sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
			sample_count = parse_be32(cur_entry->count);
			next_accum_duration = accum_duration + sample_duration * sample_count;
		}

		// skip to the key frame within the current entry
		skip_count = key_frame_index - frame_index;
		sample_count -= skip_count;
		frame_index = key_frame_index;
		accum_duration += skip_count * sample_duration;
	}

	// store the first frame info
	first_frame = frame_index;
	context->first_frame_time_offset = accum_duration;

	// calculate the end time and initial alloc size
	initial_alloc_size = 128;

	if (context->request_context->end == ULLONG_MAX)
	{
		end_time = ULLONG_MAX;

		if (entries == 1)
		{
			// optimization - pre-allocate the correct size for constant frame rate
			initial_alloc_size = parse_be32(cur_entry->count) - first_frame;
		}
	}
	else
	{
		end_time = (((uint64_t)context->request_context->end * timescale) / context->request_context->timescale);

		if (entries == 1)
		{
			// optimization - pre-allocate the correct size for constant frame rate
			sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
			if (sample_duration == 0)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: sample duration is zero (1)");
				return VOD_BAD_DATA;
			}
			initial_alloc_size = (end_time - start_time) / sample_duration + 1;
		}
	}

	// initialize the frames array
	if (initial_alloc_size > context->request_context->max_frame_count)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stts_atom: initial alloc size %uD exceeds the max frame count %uD", initial_alloc_size, context->request_context->max_frame_count);
		return VOD_BAD_DATA;
	}

	if (vod_array_init(&frames_array, context->request_context->pool, initial_alloc_size, sizeof(input_frame_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_stts_atom: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	// parse the frame durations until end time
	if (accum_duration < end_time)
	{
		for (;;)
		{
			if (sample_duration != 0 && end_time != ULLONG_MAX)
			{
				cur_count = vod_div_ceil(end_time - accum_duration, sample_duration);
				cur_count = vod_min(cur_count, sample_count);
			}
			else
			{
				cur_count = sample_count;
			}

			if (frames_array.nelts + cur_count > context->request_context->max_frame_count)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: frame count exceeds the limit %uD", context->request_context->max_frame_count);
				return VOD_BAD_DATA;
			}

			cur_frame = vod_array_push_n(&frames_array, cur_count);
			if (cur_frame == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: vod_array_push_n failed");
				return VOD_ALLOC_FAILED;
			}

			sample_count -= cur_count;
			frame_index += cur_count;
			accum_duration += cur_count * sample_duration;

			for (cur_frame_limit = cur_frame + cur_count; cur_frame < cur_frame_limit; cur_frame++)
			{
				cur_frame->duration = sample_duration;
				cur_frame->pts_delay = 0;
			}

			if (accum_duration >= end_time)
			{
				break;
			}

			// fetch next sample duration and count
			cur_entry++;
			if (cur_entry >= last_entry)
			{
				break;
			}

			sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
			sample_count = parse_be32(cur_entry->count);
		}
	}

	// parse the frame durations until the next key frame
	if (context->stss_entries != 0)
	{
		if (frames_array.nelts == 0)
		{
			context->first_frame_time_offset -= clip_from_accum_duration;
			context->request_context->start = 0;
			context->request_context->end = 0;
			return VOD_OK;
		}

		key_frame_stss_index = mp4_parser_find_stss_entry(frame_index, context->stss_start_pos, context->stss_entries);
		if (key_frame_stss_index >= context->stss_entries)
		{
			// no more keyframes, read till the end
			key_frame_index = UINT_MAX;
		}
		else
		{
			stss_entry = context->stss_start_pos + key_frame_stss_index;
			key_frame_index = parse_be32(stss_entry) - 1;
		}

		while (frame_index < key_frame_index)
		{
			sample_count = vod_min(sample_count, key_frame_index - frame_index);
			if (frames_array.nelts + sample_count > context->request_context->max_frame_count)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: frame count exceeds the limit %uD", context->request_context->max_frame_count);
				return VOD_BAD_DATA;
			}

			cur_frame = vod_array_push_n(&frames_array, sample_count);
			if (cur_frame == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: vod_array_push_n failed");
				return VOD_ALLOC_FAILED;
			}

			frame_index += sample_count;
			accum_duration += sample_count * sample_duration;
			
			for (cur_frame_limit = cur_frame + sample_count; cur_frame < cur_frame_limit; cur_frame++)
			{
				cur_frame->duration = sample_duration;
				cur_frame->pts_delay = 0;
			}

			// fetch next sample duration and count
			cur_entry++;
			if (cur_entry >= last_entry)
			{
				break;
			}

			sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
			sample_count = parse_be32(cur_entry->count);
		}

		// adjust the start / end parameters so that next streams will align according to this one
		context->request_context->timescale = timescale;
		context->request_context->start = context->first_frame_time_offset;
		if (key_frame_index != UINT_MAX)
		{
			context->request_context->end = accum_duration;
		}
		else
		{
			context->request_context->end = ULLONG_MAX;
		}
	}

	context->total_frames_duration = accum_duration - context->first_frame_time_offset;
	context->first_frame_time_offset -= clip_from_accum_duration;	
	context->frames = frames_array.elts;
	context->frame_count = frames_array.nelts;
	context->first_frame = first_frame;
	context->last_frame = first_frame + frames_array.nelts;
	
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_ctts_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	const ctts_entry_t* last_entry;
	const ctts_entry_t* cur_entry;
	input_frame_t* cur_frame = context->frames;
	input_frame_t* last_frame = cur_frame + context->frame_count;
	input_frame_t* cur_limit;
	uint32_t sample_count;
	int32_t sample_duration;
	uint32_t dts_shift = 0;
	uint32_t entries;
	uint32_t frame_index = 0;
	vod_status_t rc;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}
	

	rc = mp4_parser_validate_ctts_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	cur_entry = (const ctts_entry_t*)(atom_info->ptr + sizeof(ctts_atom_t));
	last_entry = cur_entry + entries;

	// parse the first entry
	if (cur_entry >= last_entry)
	{
		return VOD_OK;
	}

	sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
	if (sample_duration < 0)
	{
		dts_shift = vod_max(dts_shift, (uint32_t)-sample_duration);
	}

	sample_count = parse_be32(cur_entry->count);

	// jump to the first entry
	while (context->first_frame >= frame_index + sample_count)
	{
		frame_index += sample_count;

		// parse the next entry
		cur_entry++;
		if (cur_entry >= last_entry)
		{
			return VOD_OK;
		}

		sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
		if (sample_duration < 0)
		{
			dts_shift = vod_max(dts_shift, (uint32_t)-sample_duration);
		}

		sample_count = parse_be32(cur_entry->count);
	}

	// jump to the first frame
	sample_count -= (context->first_frame - frame_index);

	// parse the frames
	for (;;)
	{
		cur_limit = cur_frame + sample_count;
		cur_limit = vod_min(cur_limit, last_frame);
		for (; cur_frame < cur_limit; cur_frame++)
		{
			cur_frame->pts_delay = sample_duration;
		}

		if (cur_frame >= last_frame)
		{
			break;
		}

		// parse the next entry
		cur_entry++;
		if (cur_entry >= last_entry)
		{
			break;
		}

		sample_duration = parse_be32(cur_entry->duration) * context->parse_params.speed_denom;
		if (sample_duration < 0)
		{
			dts_shift = vod_max(dts_shift, (uint32_t)-sample_duration);
		}

		sample_count = parse_be32(cur_entry->count);
	}
	
	context->dts_shift = dts_shift;
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_stco_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	input_frame_t* cur_frame = context->frames;
	input_frame_t* last_frame = cur_frame + context->frame_count;
	uint64_t* cur_offset;
	uint64_t* last_offset;
	uint32_t entries;
	const u_char* cur_pos;
	uint32_t entry_size;
	uint64_t cur_file_offset;
	uint32_t cur_chunk_index;
	vod_status_t rc;

	rc = mp4_parser_validate_stco_data(context->request_context, atom_info, 0, &entries, &entry_size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (context->frame_count == 0)
	{
		return VOD_OK;
	}

	context->frame_offsets = vod_alloc(context->request_context->pool, context->frame_count * sizeof(uint64_t));
	if (context->frame_offsets == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_stco_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	cur_offset = context->frame_offsets;
	last_offset = context->frame_offsets + context->frame_count;

	// optimization for the case in which chunk == sample
	if (context->chunk_equals_sample)
	{
		if (entries < context->last_frame)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stco_atom: number of entries %uD smaller than last frame %uD", entries, context->last_frame);
			return VOD_BAD_DATA;
		}

		cur_pos = atom_info->ptr + sizeof(stco_atom_t) + context->first_frame * entry_size;
		if (atom_info->name == ATOM_NAME_CO64)
		{
			for (; cur_offset < last_offset; cur_offset++)
			{
				read_be64(cur_pos, *cur_offset);
			}
		}
		else
		{
			for (; cur_offset < last_offset; cur_offset++)
			{
				read_be32(cur_pos, *cur_offset);
			}
		}
		return VOD_OK;
	}

	if (last_frame[-1].key_frame >= entries)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stco_atom: number of entries %uD smaller than last chunk %uD", entries, last_frame[-1].key_frame);
		return VOD_BAD_DATA;
	}

	cur_chunk_index = cur_frame->key_frame;			// Note: we use key_frame to store the chunk index since it's temporary
	cur_pos = atom_info->ptr + sizeof(stco_atom_t) + cur_chunk_index * entry_size;
	if (atom_info->name == ATOM_NAME_CO64)
	{
		read_be64(cur_pos, cur_file_offset);
		cur_file_offset += context->first_frame_chunk_offset;
		for (; cur_offset < last_offset; cur_offset++, cur_frame++)
		{
			if (cur_frame->key_frame != cur_chunk_index)
			{
				// Note: assuming chunk indexes always grow by 1, the way stsc is encoded ensures that's always true
				cur_chunk_index = cur_frame->key_frame;
				read_be64(cur_pos, cur_file_offset);
			}
			*cur_offset = cur_file_offset;
			cur_file_offset += cur_frame->size;
		}
	}
	else
	{
		read_be32(cur_pos, cur_file_offset);
		cur_file_offset += context->first_frame_chunk_offset;
		for (; cur_offset < last_offset; cur_offset++, cur_frame++)
		{
			if (cur_frame->key_frame != cur_chunk_index)
			{
				// Note: assuming chunk indexes always grow by 1, the way stsc is encoded ensures that's always true
				cur_chunk_index = cur_frame->key_frame;
				read_be32(cur_pos, cur_file_offset);
			}
			*cur_offset = cur_file_offset;
			cur_file_offset += cur_frame->size;
		}
	}

	return VOD_OK;
}

static const u_char chunk_equals_sample_entry[] = {
	0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x01,
};

static vod_status_t 
mp4_parser_parse_stsc_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	input_frame_t* cur_frame = context->frames;
	input_frame_t* last_frame = cur_frame + context->frame_count;
	const stsc_entry_t* last_entry;
	const stsc_entry_t* cur_entry;
	uint32_t entries;
	uint32_t frame_index = 0;
	uint32_t cur_chunk;
	uint32_t next_chunk;
	uint32_t samples_per_chunk;
	uint32_t cur_entry_samples;
	uint32_t cur_sample;
	uint32_t samples_to_skip;
	vod_status_t rc;

	rc = mp4_parser_validate_stsc_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// optimization for the case where chunk == sample
	if (entries == 1 &&
		vod_memcmp(atom_info->ptr + sizeof(stsc_atom_t), chunk_equals_sample_entry, sizeof(chunk_equals_sample_entry)) == 0)
	{
		context->chunk_equals_sample = TRUE;
		context->first_chunk_frame_index = frame_index;
		return VOD_OK;
	}

	cur_entry = (const stsc_entry_t*)(atom_info->ptr + sizeof(stsc_atom_t));
	last_entry = cur_entry + entries;

	next_chunk = parse_be32(cur_entry->first_chunk);
	if (next_chunk < 1)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsc_atom: chunk index is zero (1)");
		return VOD_BAD_DATA;
	}
	next_chunk--;		// convert to 0-based

	if (frame_index < context->first_frame)
	{
		// skip to the relevant entry
		for (; cur_entry + 1 < last_entry; cur_entry++, frame_index += cur_entry_samples)
		{
			cur_chunk = next_chunk;
			next_chunk = parse_be32(cur_entry[1].first_chunk);
			samples_per_chunk = parse_be32(cur_entry->samples_per_chunk);
			if (samples_per_chunk == 0 || next_chunk < 1)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: invalid samples per chunk %uD or chunk index %uD", samples_per_chunk, next_chunk);
				return VOD_BAD_DATA;
			}
			next_chunk--;			// convert to 0-based

			if (next_chunk <= cur_chunk)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: chunk index %uD is smaller than the previous index %uD (1)", next_chunk, cur_chunk);
				return VOD_BAD_DATA;
			}

			if (next_chunk - cur_chunk > UINT_MAX / samples_per_chunk)		// integer overflow protection
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: chunk index %uD is too big for previous index %uD and samples per chunk %uD", next_chunk, cur_chunk, samples_per_chunk);
				return VOD_BAD_DATA;
			}

			cur_entry_samples = (next_chunk - cur_chunk) * samples_per_chunk;
			if (cur_entry_samples > UINT_MAX - frame_index)		// integer overflow protection
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: number of samples per entry %uD is too big", cur_entry_samples);
				return VOD_BAD_DATA;
			}

			if (frame_index + cur_entry_samples >= context->first_frame)
			{
				next_chunk = cur_chunk;		// the first frame is within the current entry, revert the value of next chunk
				break;
			}
		}
	}

	for (; cur_entry < last_entry; cur_entry++)
	{
		// get and validate samples_per_chunk, cur_chunk and next_chunk
		samples_per_chunk = parse_be32(cur_entry->samples_per_chunk);
		if (samples_per_chunk == 0)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stsc_atom: samples per chunk is zero");
			return VOD_BAD_DATA;
		}
		cur_chunk = next_chunk;
		if (cur_entry + 1 < last_entry)
		{
			next_chunk = parse_be32(cur_entry[1].first_chunk);
			if (next_chunk < 1)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: chunk index is zero (2)");
				return VOD_BAD_DATA;
			}
			next_chunk--;			// convert to 0-based
			if (next_chunk <= cur_chunk)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: chunk index %uD is smaller than the previous index %uD (2)", next_chunk, cur_chunk);
				return VOD_BAD_DATA;
			}
		}
		else
		{
			next_chunk = UINT_MAX;
		}

		// map the frames to the chunk they reside in
		for (; cur_chunk < next_chunk; cur_chunk++)
		{
			cur_sample = samples_per_chunk;
			if (frame_index < context->first_frame)
			{
				// skip samples until we get to the first frame
				samples_to_skip = vod_min(context->first_frame - frame_index, samples_per_chunk);
				cur_sample -= samples_to_skip;
				frame_index += samples_to_skip;
			}

			for (; cur_sample; cur_sample--, frame_index++)
			{
				if (frame_index == context->first_frame)
				{
					context->first_chunk_frame_index = frame_index - (samples_per_chunk - cur_sample);
				}

				if (cur_frame >= last_frame)
				{
					return VOD_OK;
				}

				// Note: using the key_frame field to hold the chunk index in order to avoid allocating
				//		extra room for it (it's temporary)
				cur_frame->key_frame = cur_chunk;
				cur_frame++;
			}
		}
	}

	// unexpected - didn't reach the last frame
	vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
		"mp4_parser_parse_stsc_atom: failed to get the chunk indexes for all frames");
	return VOD_BAD_DATA;
}

static vod_status_t
mp4_parser_parse_stsz_atom_total_size_estimate_only(atom_info_t* atom_info, frames_parse_context_t* context)
{
	uint32_t uniform_size;
	uint32_t test_entries;
	uint32_t entries;
	unsigned field_size;
	const u_char* cur_pos;
	const u_char* end_pos;
	vod_status_t rc;

	rc = mp4_parser_validate_stsz_atom(context->request_context, atom_info, context->last_frame, &uniform_size, &field_size, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (uniform_size != 0)
	{
		context->total_frames_size = ((uint64_t)entries) * uniform_size;
		return VOD_OK;
	}

	test_entries = vod_min(entries, MAX_TOTAL_SIZE_TEST_SAMPLES);

	cur_pos = atom_info->ptr + sizeof(stsz_atom_t);
	switch (field_size)
	{
	case 32:
		end_pos = cur_pos + test_entries * sizeof(uint32_t);
		for (; cur_pos < end_pos; cur_pos += sizeof(uint32_t))
		{
			context->total_frames_size += parse_be32(cur_pos);
		}
		break;

	case 16:
		end_pos = cur_pos + test_entries * sizeof(uint16_t);
		for (; cur_pos < end_pos; cur_pos += sizeof(uint16_t))
		{
			context->total_frames_size += parse_be16(cur_pos);
		}
		break;

	case 8:
		end_pos = cur_pos + test_entries;
		for (; cur_pos < end_pos; cur_pos += sizeof(uint8_t))
		{
			context->total_frames_size += *cur_pos;
		}
		break;

	case 4:
		// TODO: implement this

	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsz_atom_total_size_estimate_only: unsupported field size %ud", field_size);
		return VOD_BAD_DATA;
	}

	if (test_entries != entries)
	{
		context->total_frames_size = (context->total_frames_size * entries) / test_entries;
	}

	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_stsz_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	input_frame_t* cur_frame = context->frames;
	input_frame_t* last_frame = cur_frame + context->frame_count;
	uint32_t first_frame_index_in_chunk = context->first_frame - context->first_chunk_frame_index;
	const u_char* cur_pos;
	uint32_t uniform_size;
	uint32_t entries;
	unsigned field_size;
	vod_status_t rc;

	rc = mp4_parser_validate_stsz_atom(context->request_context, atom_info, context->last_frame, &uniform_size, &field_size, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (uniform_size != 0)
	{
		context->first_frame_chunk_offset = ((uint64_t)first_frame_index_in_chunk) * uniform_size;
		for (; cur_frame < last_frame; cur_frame++)
		{
			cur_frame->size = uniform_size;
		}
		context->total_frames_size += uniform_size * context->frame_count;
		return VOD_OK;
	}
	
	switch (field_size)
	{
	case 32:
		cur_pos = atom_info->ptr + sizeof(stsz_atom_t) + context->first_chunk_frame_index * sizeof(uint32_t);
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--, cur_pos += sizeof(uint32_t))
		{
			context->first_frame_chunk_offset += parse_be32(cur_pos);
		}
		for (; cur_frame < last_frame; cur_frame++)
		{
			read_be32(cur_pos, cur_frame->size);
			if (cur_frame->size > MAX_FRAME_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsz_atom: frame size %uD too big", cur_frame->size);
				return VOD_BAD_DATA;
			}
			context->total_frames_size += cur_frame->size;
		}
		break;

	case 16:
		cur_pos = atom_info->ptr + sizeof(stsz_atom_t) + context->first_chunk_frame_index * sizeof(uint16_t);
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--, cur_pos += sizeof(uint16_t))
		{
			context->first_frame_chunk_offset += parse_be16(cur_pos);
		}
		for (; cur_frame < last_frame; cur_frame++)
		{
			read_be16(cur_pos, cur_frame->size);
			// Note: no need to validate the size here, since MAX_UINT16 < MAX_FRAME_SIZE
			context->total_frames_size += cur_frame->size;
		}
		break;
		
	case 8:
		cur_pos = atom_info->ptr + sizeof(stsz_atom_t) + context->first_chunk_frame_index;
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--)
		{
			context->first_frame_chunk_offset += *cur_pos++;
		}
		for (; cur_frame < last_frame; cur_frame++)
		{
			cur_frame->size = *cur_pos++;
			// Note: no need to validate the size here, since MAX_UINT8 < MAX_FRAME_SIZE
			context->total_frames_size += cur_frame->size;
		}
		break;
		
	case 4:
		// TODO: implement this
		
	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsz_atom: unsupported field size %ud", field_size);
		return VOD_BAD_DATA;
	}
	
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_stss_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	input_frame_t* cur_frame = context->frames;
	input_frame_t* last_frame = cur_frame + context->frame_count;
	const uint32_t* start_pos;
	const uint32_t* cur_pos;
	const uint32_t* end_pos;
	uint32_t entries;
	uint32_t frame_index;
	vod_status_t rc;

	for (; cur_frame < last_frame; cur_frame++)
	{
		cur_frame->key_frame = FALSE;
	}
	
	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}

	rc = mp4_parser_validate_stss_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	start_pos = (const uint32_t*)(atom_info->ptr + sizeof(stss_atom_t));
	end_pos = start_pos + entries;

	if (context->stss_start_index == 0 && context->first_frame > 0)
	{
		context->stss_start_index = mp4_parser_find_stss_entry(context->first_frame, start_pos, entries);
	}
	cur_pos = start_pos + context->stss_start_index;
	for (; cur_pos < end_pos; cur_pos++)
	{
		frame_index = parse_be32(cur_pos) - 1;		// 1 based index
		if (frame_index < context->first_frame)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stss_atom: frame indexes are not strictly ascending");
			return VOD_BAD_DATA;
		}

		if (frame_index >= context->last_frame)
		{
			break;
		}

		cur_frame = &context->frames[frame_index - context->first_frame];
		if (!cur_frame->key_frame)		// increment only once in case a frame is listed twice
		{
			cur_frame->key_frame = TRUE;
			context->key_frame_count++;
		}
	}
	
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_video_extra_data_atom(void* ctx, atom_info_t* atom_info)
{
	metadata_parse_context_t* context = (metadata_parse_context_t*)ctx;
	
	switch (atom_info->name)
	{
	case ATOM_NAME_AVCC:
	case ATOM_NAME_HVCC:
		break;			// handled outside the switch

	default:
		return VOD_OK;
	}
	
	context->media_info.extra_data_size = atom_info->size;
	context->media_info.extra_data = atom_info->ptr;
	
	return VOD_OK;
}

// Note: no validations in the functions below - the stream functions will make sure 
//		we don't overflow the input buffer
static int 
mp4_parser_read_descriptor_length(simple_read_stream_t* stream)			// ff_mp4_read_descr_len
{
	int len = 0;
	int count = 4;
	while (count--) 
	{
		int c = read_stream_get_byte(stream);
		len = (len << 7) | (c & 0x7f);
		if (!(c & 0x80))
		{
			break;
		}
	}
	return len;
}

static int 
mp4_parser_read_descriptor(simple_read_stream_t* stream, int *tag)			// ff_mp4_read_descr
{
	*tag = read_stream_get_byte(stream);
	return mp4_parser_read_descriptor_length(stream);
}

static void 
mp4_parser_parse_es_descriptor(simple_read_stream_t* stream)							// ff_mp4_parse_es_descr
{
	int flags;
	
	read_stream_skip(stream, 2);
	flags = read_stream_get_byte(stream);
	if (flags & 0x80) //streamDependenceFlag
	{
		read_stream_skip(stream, 2);
	}
	if (flags & 0x40) //URL_Flag
	{
		int len = read_stream_get_byte(stream);
		read_stream_skip(stream, len);
	}
	if (flags & 0x20) //OCRstreamFlag
	{
		read_stream_skip(stream, 2);
	}
}

static vod_status_t 
mp4_parser_read_config_descriptor(metadata_parse_context_t* context, simple_read_stream_t* stream)		// ff_mp4_read_dec_config_descr
{
	unsigned len;
	int tag;

	context->media_info.u.audio.object_type_id = read_stream_get_byte(stream);
	read_stream_skip(stream, member_size(config_descr_t, stream_type) + member_size(config_descr_t, buffer_size) + member_size(config_descr_t, max_bitrate));
	context->media_info.bitrate = read_stream_get_be32(stream);

	len = mp4_parser_read_descriptor(stream, &tag);
	if (tag == MP4DecSpecificDescrTag)
	{
		if (len > (unsigned)(stream->end_pos - stream->cur_pos))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_read_config_descriptor: tag length %ud too big", len);
			return VOD_BAD_DATA;
		}
		context->media_info.extra_data_size = len;
		context->media_info.extra_data = stream->cur_pos;
	}
	
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_audio_esds_atom(void* ctx, atom_info_t* atom_info)
{
	metadata_parse_context_t* context = (metadata_parse_context_t*)ctx;
	simple_read_stream_t stream;
	int tag;
	vod_status_t rc;
	
	if (atom_info->name != ATOM_NAME_ESDS)
	{
		return VOD_OK;
	}
	
	stream.cur_pos = atom_info->ptr;
	stream.end_pos = stream.cur_pos + atom_info->size;
	
	read_stream_skip(&stream, 4);		// version + flags
	mp4_parser_read_descriptor(&stream, &tag);
	if (tag == MP4ESDescrTag) 
	{
		mp4_parser_parse_es_descriptor(&stream);
	}
	else
	{
		read_stream_skip(&stream, 2);		// ID
	}

	mp4_parser_read_descriptor(&stream, &tag);
	if (tag == MP4DecConfigDescrTag)
	{
		rc = mp4_parser_read_config_descriptor(context, &stream);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_audio_extra_data_atom(void* ctx, atom_info_t* atom_info)
{
	metadata_parse_context_t* context = (metadata_parse_context_t*)ctx;
	vod_status_t rc;

	if (atom_info->name == ATOM_NAME_WAVE && atom_info->size > 8)
	{
		rc = mp4_parser_parse_atoms(
			context->request_context, 
			atom_info->ptr, 
			atom_info->size, 
			TRUE, 
			mp4_parser_parse_audio_esds_atom, 
			context);
	}
	else
	{
		rc = mp4_parser_parse_audio_esds_atom(ctx, atom_info);
	}

	return rc;
}

static const u_char* 
mp4_parser_skip_stsd_atom_video(const u_char* cur_pos, const u_char* end_pos, metadata_parse_context_t* context)
{
	stsd_video_t* video;
	uint16_t bits_per_coded_sample;
	uint16_t colortable_id;
	uint16_t color_depth;
	uint16_t color_greyscale;
	stsd_video_palette_header_t* palette;
	uint32_t color_start;
	uint16_t color_end;

	if (cur_pos + sizeof(*video) > end_pos)
	{
		return NULL;
	}
		
	video = (stsd_video_t*)cur_pos;
	cur_pos += sizeof(*video);

	context->media_info.u.video.width = parse_be16(video->width);
	context->media_info.u.video.height = parse_be16(video->height);

	bits_per_coded_sample = parse_be16(video->bits_per_coded_sample);
	colortable_id = parse_be16(video->colortable_id);		
	color_depth	 = bits_per_coded_sample & 0x1F;
	color_greyscale = bits_per_coded_sample & 0x20;
	
	if ((color_depth == 2) || (color_depth == 4) || (color_depth == 8)) 
	{
		if (!color_greyscale && !colortable_id) 
		{
			if (cur_pos + sizeof(*palette) > end_pos)
			{
				return NULL;
			}

			palette = (stsd_video_palette_header_t*)cur_pos;
			cur_pos += sizeof(*palette);
			
			color_start = parse_be32(palette->color_start);
			color_end = parse_be16(palette->color_end);
			if ((color_start <= 255) && (color_end <= 255) && color_end >= color_start) 
			{
				cur_pos += (color_end - color_start + 1) * sizeof(stsd_video_palette_entry_t);
			}
		}
	}
	
	return cur_pos;
}

static const u_char* 
mp4_parser_skip_stsd_atom_audio(const u_char* cur_pos, const u_char* end_pos, metadata_parse_context_t* context)
{
	stsd_audio_t* audio;
	uint16_t version;

	if (cur_pos + sizeof(stsd_audio_t) > end_pos)
	{
		return NULL;
	}
	
	audio = (stsd_audio_t*)cur_pos;
	cur_pos += sizeof(stsd_audio_t);
	
	context->media_info.u.audio.channels = parse_be16(audio->channels);
	context->media_info.u.audio.bits_per_sample = parse_be16(audio->bits_per_coded_sample);
	context->media_info.u.audio.packet_size = parse_be16(audio->packet_size);
	context->media_info.u.audio.sample_rate = parse_be32(audio->sample_rate) >> 16;
	version = parse_be16(audio->version);

	switch (version)
	{
	case 1:
		cur_pos += sizeof(stsd_audio_qt_version_1_t);
		break;

	case 2:
		cur_pos += sizeof(stsd_audio_qt_version_2_t);
		break;
	}

	return cur_pos;
}

static vod_status_t 
mp4_parser_parse_stsd_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const stsd_atom_t* atom = (const stsd_atom_t*)atom_info->ptr;
	uint32_t entries;
	const u_char* cur_pos;
	const u_char* end_pos;
	uint32_t size;
	const u_char* (*skip_function)(const u_char* cur_pos, const u_char* end_pos, metadata_parse_context_t* context);
	parse_atoms_callback_t parse_function;
	vod_status_t rc;

	switch (context->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		skip_function = mp4_parser_skip_stsd_atom_video;
		parse_function = mp4_parser_parse_video_extra_data_atom;
		break;

	case MEDIA_TYPE_AUDIO:
		skip_function = mp4_parser_skip_stsd_atom_audio;
		parse_function = mp4_parser_parse_audio_extra_data_atom;		
		break;
		
	default:
		return VOD_OK;
	}
	
	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsd_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	cur_pos = atom_info->ptr + sizeof(*atom);
	end_pos = atom_info->ptr + atom_info->size;
	for (entries = parse_be32(atom->entries); entries; entries--)
	{
		if (cur_pos + sizeof(stsd_entry_header_t) > end_pos)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stsd_atom: not enough room for stsd entry");
			return VOD_BAD_DATA;
		}
	
		context->media_info.format = parse_le32(((stsd_entry_header_t*)cur_pos)->format);
		size = parse_be32(((stsd_entry_header_t*)cur_pos)->size);
		cur_pos += sizeof(stsd_entry_header_t);
		if (size >= 16)
		{
			cur_pos += sizeof(stsd_large_entry_header_t);
		}
		
		cur_pos = skip_function(cur_pos, end_pos, context);
		if (cur_pos == NULL)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stsd_atom: failed to skip audio/video data");
			return VOD_BAD_DATA;
		}
	}

	if (cur_pos > end_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsd_atom: stream overflow before reaching extra data");
		return VOD_BAD_DATA;
	}
	
	if (cur_pos < end_pos)
	{
		vod_log_buffer(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0, "mp4_parser_parse_stsd_atom: extra data ", cur_pos, end_pos - cur_pos);
		
		rc = mp4_parser_parse_atoms(context->request_context, cur_pos, end_pos - cur_pos, TRUE, parse_function, context);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return VOD_OK;
}

static bool_t
does_stream_exist(stream_comparator_t compare_streams, void* context, media_info_t* media_info, vod_array_t* streams)
{
	mpeg_stream_metadata_t* streams_start = (mpeg_stream_metadata_t*)streams->elts;
	mpeg_stream_metadata_t* streams_end = streams_start + streams->nelts;
	mpeg_stream_metadata_t* cur_stream;

	for (cur_stream = streams_start; cur_stream < streams_end; cur_stream++)
	{
		if (compare_streams(context, &cur_stream->media_info, media_info))
		{
			return TRUE;
		}
	}
	return FALSE;
}

static vod_status_t 
mp4_parser_process_moov_atom_callback(void* ctx, atom_info_t* atom_info)
{
	process_moov_context_t* context = (process_moov_context_t*)ctx;
	save_relevant_atoms_context_t save_atoms_context;
	trak_atom_infos_t trak_atom_infos;
	metadata_parse_context_t metadata_parse_context;
	mpeg_stream_base_metadata_t* result_stream;
	mpeg_base_metadata_t* result = context->result;
	uint32_t duration_millis;
	uint32_t track_index;
	bool_t format_supported = FALSE;
	vod_status_t rc;
	bool_t is_avc = FALSE;
	u_char* new_extra_data;
	u_char* atom_data;
	int parse_type;

	switch (atom_info->name)
	{
	case ATOM_NAME_MVHD:
		if ((context->request_context->parse_type & PARSE_FLAG_SAVE_RAW_ATOMS) != 0)
		{
			result->mvhd_atom.size = atom_info->header_size + atom_info->size;
			result->mvhd_atom.header_size = atom_info->header_size;
			atom_data = vod_alloc(context->request_context->pool, result->mvhd_atom.size);
			if (atom_data == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mp4_parser_process_moov_atom_callback: vod_alloc failed");
				return VOD_ALLOC_FAILED;
			}

			vod_memcpy(atom_data, atom_info->ptr - atom_info->header_size, result->mvhd_atom.size);
			result->mvhd_atom.ptr = atom_data;
		}
		return VOD_OK;

	case ATOM_NAME_TRAK:
		break;		// handled outside the switch

	default:
		return VOD_OK;
	}

	// find required trak atoms
	vod_memzero(&trak_atom_infos, sizeof(trak_atom_infos));
	save_atoms_context.relevant_atoms = relevant_atoms_trak;
	save_atoms_context.result = &trak_atom_infos;
	save_atoms_context.request_context = context->request_context;
	rc = mp4_parser_parse_atoms(context->request_context, atom_info->ptr, atom_info->size, TRUE, &mp4_parser_save_relevant_atoms_callback, &save_atoms_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the media info
	vod_memzero(&metadata_parse_context, sizeof(metadata_parse_context));
	metadata_parse_context.request_context = context->request_context;
	metadata_parse_context.parse_params = context->parse_params;
	rc = mp4_parser_parse_hdlr_atom(&trak_atom_infos.hdlr, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the codec type and extra data
	rc = mp4_parser_parse_stsd_atom(&trak_atom_infos.stsd, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// make sure the codec is supported
	switch (metadata_parse_context.media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		switch (metadata_parse_context.media_info.format)
		{
		case FORMAT_AVC1:
		case FORMAT_h264:
		case FORMAT_H264:
			is_avc = TRUE;
		case FORMAT_HEV1:
		case FORMAT_HVC1:
			format_supported = TRUE;
			break;
		}
		break;

	case MEDIA_TYPE_AUDIO:
		if (metadata_parse_context.media_info.format == FORMAT_MP4A)
		{
			switch (metadata_parse_context.media_info.u.audio.object_type_id)
			{
			case 0x40:
			case 0x66:
			case 0x67:
			case 0x68:
				format_supported = TRUE;
				break;
			}
		}
		break;
	}

	if (!format_supported)
	{
		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: unsupported format - media type %uD format 0x%uxD object type id 0x%uxD",
			metadata_parse_context.media_info.media_type, metadata_parse_context.media_info.format, (uint32_t)metadata_parse_context.media_info.u.audio.object_type_id);
		return VOD_OK;
	}

	// make sure we got the extra data
	if (metadata_parse_context.media_info.extra_data == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: no extra data was parsed for track");
		return VOD_BAD_DATA;
	}

	// get the track id
	rc = mp4_parser_parse_tkhd_atom(&trak_atom_infos.tkhd, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// check whether we should include this track
	track_index = context->track_indexes[metadata_parse_context.media_info.media_type]++;
	if ((context->parse_params.required_tracks_mask[metadata_parse_context.media_info.media_type] & (1 << track_index)) == 0)
	{
		return VOD_OK;
	}

	// get the duration
	rc = mp4_parser_parse_mdhd_atom(&trak_atom_infos.mdhd, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// apply the clipping to the duration
	if (metadata_parse_context.media_info.duration_millis <= context->parse_params.clip_from)
	{
		return VOD_OK;
	}

	duration_millis = vod_min(metadata_parse_context.media_info.duration_millis, context->parse_params.clip_to) - context->parse_params.clip_from;
	if (duration_millis != metadata_parse_context.media_info.duration_millis)
	{
		metadata_parse_context.media_info.duration_millis = duration_millis;
		metadata_parse_context.media_info.duration = rescale_time(duration_millis, 1000, metadata_parse_context.media_info.timescale);
	}

	// get the codec name
	parse_type = context->request_context->parse_type;

	if ((parse_type & PARSE_FLAG_CODEC_NAME) != 0)
	{
		metadata_parse_context.media_info.codec_name.data = vod_alloc(context->request_context->pool, MAX_CODEC_NAME_SIZE);
		if (metadata_parse_context.media_info.codec_name.data == NULL)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_process_moov_atom_callback: failed to allocate codec name");
			return VOD_ALLOC_FAILED;
		}

		switch (metadata_parse_context.media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			rc = codec_config_get_video_codec_name(context->request_context, &metadata_parse_context.media_info);
			break;

		case MEDIA_TYPE_AUDIO:
			rc = codec_config_get_audio_codec_name(context->request_context, &metadata_parse_context.media_info);
			break;
		}

		if (rc != VOD_OK)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"mp4_parser_process_moov_atom_callback: failed to get codec name");
			return rc;
		}
	}

	// parse / copy the extra data
	if (metadata_parse_context.parse_params.speed_nom != metadata_parse_context.parse_params.speed_denom &&
		metadata_parse_context.media_info.media_type == MEDIA_TYPE_AUDIO)
	{
		// always need the extra data when changing the speed of an audio stream (used by audio_filter)
		parse_type |= PARSE_FLAG_EXTRA_DATA;
	}

	if ((parse_type & (PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_EXTRA_DATA_SIZE)) != 0)
	{
		// TODO: add HEVC support

		if (is_avc && (parse_type & PARSE_FLAG_EXTRA_DATA_PARSE) != 0)
		{
			// extract the nal units
			rc = codec_config_avcc_get_nal_units(
				context->request_context,
				metadata_parse_context.media_info.extra_data,
				metadata_parse_context.media_info.extra_data_size,
				(parse_type & PARSE_FLAG_EXTRA_DATA) == 0,
				&new_extra_data,
				&metadata_parse_context.media_info.extra_data_size);
			if (rc != VOD_OK)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mp4_parser_process_moov_atom_callback: codec_config_avcc_get_nal_units failed %i", rc);
				return rc;
			}

			metadata_parse_context.media_info.u.video.nal_packet_size_length = (((avcc_config_t*)metadata_parse_context.media_info.extra_data)->nula_length_size & 0x3) + 1;
			metadata_parse_context.media_info.extra_data = new_extra_data;
		}
		else if ((parse_type & PARSE_FLAG_EXTRA_DATA) != 0)
		{
			// copy the extra data, we should not reference the moov buffer after we finish parsing
			new_extra_data = vod_alloc(context->request_context->pool, metadata_parse_context.media_info.extra_data_size);
			if (new_extra_data == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mp4_parser_process_moov_atom_callback: vod_alloc failed");
				return VOD_ALLOC_FAILED;
			}
			vod_memcpy(new_extra_data, metadata_parse_context.media_info.extra_data, metadata_parse_context.media_info.extra_data_size);
			metadata_parse_context.media_info.extra_data = new_extra_data;
		}
		else
		{
			metadata_parse_context.media_info.extra_data = NULL;
		}
	}
	else
	{
		metadata_parse_context.media_info.extra_data = NULL;
		metadata_parse_context.media_info.extra_data_size = 0;
	}

	// add to the result array
	if (result->streams.nelts >= MAX_STREAM_COUNT)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: stream count exceeded the limit");
		return VOD_BAD_REQUEST;
	}

	result_stream = vod_array_push(&result->streams);
	if (result_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}

	metadata_parse_context.media_info.speed_nom = context->parse_params.speed_nom;
	metadata_parse_context.media_info.speed_denom = context->parse_params.speed_denom;

	result_stream->trak_atom_infos = trak_atom_infos;
	result_stream->media_info = metadata_parse_context.media_info;
	result_stream->file_info = *context->file_info;
	result_stream->track_index = track_index;

	// update max duration / track index
	if (result->duration == 0 ||
		result->duration * metadata_parse_context.media_info.timescale < metadata_parse_context.media_info.duration * result->timescale)
	{
		result->duration_millis = metadata_parse_context.media_info.duration_millis;
		result->duration = metadata_parse_context.media_info.duration;
		result->timescale = metadata_parse_context.media_info.timescale;
	}

	if (track_index > result->max_track_index)
	{
		result->max_track_index = track_index;
	}

	return VOD_OK;
}

vod_status_t
mp4_parser_get_ftyp_atom_into(request_context_t* request_context, const u_char* buffer, size_t buffer_size, const u_char** ptr, size_t* size)
{
	atom_info_t find_context = { NULL, 0, ATOM_NAME_FTYP, 0 };

	// not checking the result of mp4_parser_parse_atoms since we always stop the enumeration here
	mp4_parser_parse_atoms(request_context, buffer, buffer_size, FALSE, &mp4_parser_find_atom_callback, &find_context);
	if (find_context.ptr == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_get_ftyp_atom_into: failed to parse any atoms");
		return VOD_BAD_DATA;
	}

	*ptr = find_context.ptr;
	*size = find_context.size;

	return VOD_OK;
}

vod_status_t 
mp4_parser_get_moov_atom_info(request_context_t* request_context, const u_char* buffer, size_t buffer_size, off_t* offset, size_t* size)
{
	atom_info_t find_moov_context = { NULL, 0, ATOM_NAME_MOOV, 0 };

	// not checking the result of mp4_parser_parse_atoms since we always stop the enumeration here
	mp4_parser_parse_atoms(request_context, buffer, buffer_size, FALSE, &mp4_parser_find_atom_callback, &find_moov_context);
	if (find_moov_context.ptr == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_get_moov_atom_info: failed to parse any atoms");
		return VOD_BAD_DATA;
	}
		
	*offset = find_moov_context.ptr - buffer;
	*size = find_moov_context.size;
	
	return VOD_OK;
}

static const trak_atom_parser_t trak_atom_parsers[] = {
	// order is important
	{ mp4_parser_parse_stts_atom,							offsetof(trak_atom_infos_t, stts), PARSE_FLAG_FRAMES_DURATION },
	{ mp4_parser_parse_stts_atom_frame_duration_only,		offsetof(trak_atom_infos_t, stts), PARSE_FLAG_DURATION_LIMITS },
	{ mp4_parser_parse_ctts_atom,							offsetof(trak_atom_infos_t, ctts), PARSE_FLAG_FRAMES_PTS_DELAY },
	{ mp4_parser_parse_stsc_atom,							offsetof(trak_atom_infos_t, stsc), PARSE_FLAG_FRAMES_OFFSET },
	{ mp4_parser_parse_stsz_atom,							offsetof(trak_atom_infos_t, stsz), PARSE_FLAG_FRAMES_SIZE },
	{ mp4_parser_parse_stsz_atom_total_size_estimate_only,	offsetof(trak_atom_infos_t, stsz), PARSE_FLAG_TOTAL_SIZE_ESTIMATE },
	{ mp4_parser_parse_stco_atom,							offsetof(trak_atom_infos_t, stco), PARSE_FLAG_FRAMES_OFFSET},
	{ mp4_parser_parse_stss_atom,							offsetof(trak_atom_infos_t, stss), PARSE_FLAG_FRAMES_IS_KEY },
	{ NULL, 0, 0 }
};

vod_status_t
mp4_parser_init_mpeg_metadata(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata)
{
	if (vod_array_init(&mpeg_metadata->streams, request_context->pool, 2, sizeof(mpeg_stream_metadata_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_init_mpeg_metadata: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

vod_status_t 
mp4_parser_parse_basic_metadata(
	request_context_t* request_context,
	mpeg_parse_params_t* parse_params,
	const u_char* buffer,
	size_t size,
	file_info_t* file_info,
	mpeg_base_metadata_t* result)
{
	process_moov_context_t process_moov_context;
	vod_status_t rc;

	vod_memzero(result, sizeof(*result));
	if (vod_array_init(&result->streams, request_context->pool, 2, sizeof(mpeg_stream_base_metadata_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_parse_basic_metadata: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	process_moov_context.request_context = request_context;
	process_moov_context.parse_params = *parse_params;
	vod_memzero(process_moov_context.track_indexes, sizeof(process_moov_context.track_indexes));
	process_moov_context.file_info = file_info;
	process_moov_context.result = result;

	rc = mp4_parser_parse_atoms(request_context, buffer, size, TRUE, &mp4_parser_process_moov_atom_callback, &process_moov_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
mp4_parser_copy_raw_atoms(request_context_t* request_context, raw_atom_t* dest, trak_atom_infos_t* source)
{
	const raw_atom_mapping_t* end = raw_atom_mapping + sizeof(raw_atom_mapping) / sizeof(raw_atom_mapping[0]);
	const raw_atom_mapping_t* cur;
	atom_info_t* atom_info;
	raw_atom_t* raw_atom;
	size_t total_size = 0;
	u_char* p;

	for (cur = raw_atom_mapping; cur < end; cur++)
	{
		atom_info = (atom_info_t*)((u_char*)source + cur->atom_info_offset);
		raw_atom = dest + cur->raw_atom_index;

		raw_atom->size = atom_info->header_size + atom_info->size;
		raw_atom->header_size = atom_info->header_size;
		total_size += raw_atom->size;
	}

	p = vod_alloc(request_context->pool, total_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_copy_raw_atoms: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	for (cur = raw_atom_mapping; cur < end; cur++)
	{
		atom_info = (atom_info_t*)((u_char*)source + cur->atom_info_offset);
		raw_atom = dest + cur->raw_atom_index;

		raw_atom->ptr = p;
		p = vod_copy(p, atom_info->ptr - atom_info->header_size, raw_atom->size);
	}

	return VOD_OK;
}

static int
mp4_parser_compare_streams(const void *s1, const void *s2)
{
	mpeg_stream_base_metadata_t* stream1 = (mpeg_stream_base_metadata_t*)s1;
	mpeg_stream_base_metadata_t* stream2 = (mpeg_stream_base_metadata_t*)s2;

	if (stream1->media_info.media_type != stream2->media_info.media_type)
	{
		return stream1->media_info.media_type - stream2->media_info.media_type;
	}

	return stream1->track_index - stream2->track_index;
}

vod_status_t 
mp4_parser_parse_frames(
	request_context_t* request_context,
	mpeg_base_metadata_t* base,
	mpeg_parse_params_t* parse_params,
	bool_t align_segments_to_key_frames,
	mpeg_metadata_t* result)
{
	mpeg_stream_base_metadata_t* first_stream = (mpeg_stream_base_metadata_t*)base->streams.elts;
	mpeg_stream_base_metadata_t* last_stream = first_stream + base->streams.nelts;
	mpeg_stream_base_metadata_t* cur_stream;
	frames_parse_context_t context;
	mpeg_stream_metadata_t* result_stream;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	const trak_atom_parser_t* cur_parser;
	vod_status_t rc;
	uint32_t media_type;

	result->mvhd_atom = base->mvhd_atom;

	// in case we need to parse the frame sizes, we already find the total size
	if ((request_context->parse_type & PARSE_FLAG_FRAMES_SIZE) != 0)
	{
		request_context->parse_type &= ~PARSE_FLAG_TOTAL_SIZE_ESTIMATE;
	}

	// sort the streams - video first
	qsort(first_stream, base->streams.nelts, sizeof(*first_stream), mp4_parser_compare_streams);

	for (cur_stream = first_stream; cur_stream < last_stream; cur_stream++)
	{
		media_type = cur_stream->media_info.media_type;

		// parse the rest of the trak atoms
		vod_memzero(&context, sizeof(context));
		context.request_context = request_context;
		context.media_info = &cur_stream->media_info;
		context.parse_params = *parse_params;

		if (cur_stream == first_stream &&
			media_type == MEDIA_TYPE_VIDEO &&
			cur_stream->trak_atom_infos.stss.size != 0 &&
			align_segments_to_key_frames)
		{
			rc = mp4_parser_validate_stss_atom(context.request_context, &cur_stream->trak_atom_infos.stss, &context.stss_entries);
			if (rc != VOD_OK)
			{
				return rc;
			}

			context.stss_start_pos = (const uint32_t*)(cur_stream->trak_atom_infos.stss.ptr + sizeof(stss_atom_t));
		}

		for (cur_parser = trak_atom_parsers; cur_parser->parse; cur_parser++)
		{
			if ((request_context->parse_type & cur_parser->flag) == 0)
			{
				continue;
			}

			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "mp4_parser_parse_frames: running parser 0x%xD", cur_parser->flag);
			rc = cur_parser->parse((atom_info_t*)((u_char*)&cur_stream->trak_atom_infos + cur_parser->offset), &context);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		// estimate the bitrate from frame size if no bitrate was read from the file
		if (cur_stream->media_info.bitrate == 0 && cur_stream->media_info.duration > 0)
		{
			cur_stream->media_info.bitrate = (uint32_t)((context.total_frames_size * cur_stream->media_info.timescale * 8) / cur_stream->media_info.duration);
		}

		// skip the stream if it is a duplicate of an existing stream
		if (request_context->stream_comparator != NULL &&
			does_stream_exist(
				request_context->stream_comparator,
				request_context->stream_comparator_context,
				&cur_stream->media_info,
				&result->streams))
		{
			vod_free(request_context->pool, context.frames);
			vod_free(request_context->pool, context.frame_offsets);
			continue;
		}

		result_stream = vod_array_push(&result->streams);
		if (result_stream == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_parser_parse_frames: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}

		// copy the result
		result_stream->media_info = cur_stream->media_info;
		result_stream->file_info = cur_stream->file_info;
		result_stream->track_index = cur_stream->track_index;
		result_stream->frames = context.frames;
		result_stream->frames_file_index = cur_stream->file_info.file_index;
		result_stream->frame_offsets = context.frame_offsets;
		result_stream->frame_count = context.frame_count;
		result_stream->key_frame_count = context.key_frame_count;
		result_stream->total_frames_size = context.total_frames_size;
		result_stream->total_frames_duration = context.total_frames_duration;
		result_stream->first_frame_index = context.first_frame;
		result_stream->first_frame_time_offset = context.first_frame_time_offset;
		result_stream->clip_from_frame_offset = context.clip_from_frame_offset;

		// copy raw atoms
		if ((request_context->parse_type & PARSE_FLAG_SAVE_RAW_ATOMS) != 0)
		{
			rc = mp4_parser_copy_raw_atoms(request_context, result_stream->raw_atoms, &cur_stream->trak_atom_infos);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		// add the dts_shift to the pts_delay
		cur_frame = context.frames;
		last_frame = cur_frame + context.frame_count;
		for (; cur_frame < last_frame; cur_frame++)
		{
			cur_frame->pts_delay += context.dts_shift;
		}

		// update max duration / track index
		if (result->longest_stream[media_type] == NULL ||
			result->longest_stream[media_type]->media_info.duration * cur_stream->media_info.timescale < cur_stream->media_info.duration * result->longest_stream[media_type]->media_info.timescale)
		{
			result->longest_stream[media_type] = result_stream;

			if (result->duration == 0 ||
				result->duration * cur_stream->media_info.timescale < cur_stream->media_info.duration * result->timescale)
			{
				result->duration_millis = cur_stream->media_info.duration_millis;
				result->duration = cur_stream->media_info.duration;
				result->timescale = cur_stream->media_info.timescale;
			}
		}

		if (cur_stream->track_index > result->max_track_index)
		{
			result->max_track_index = cur_stream->track_index;
		}

		result->stream_count[media_type]++;
		if (media_type == MEDIA_TYPE_VIDEO)
		{
			result->video_key_frame_count += context.key_frame_count;
		}
	}

	return VOD_OK;
}

void 
mp4_parser_finalize_mpeg_metadata(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata)
{
	mpeg_metadata->first_stream = (mpeg_stream_metadata_t*)mpeg_metadata->streams.elts;
	mpeg_metadata->last_stream = mpeg_metadata->first_stream + mpeg_metadata->streams.nelts;
}
