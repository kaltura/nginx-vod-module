#include <limits.h>
#include "mp4_parser.h"
#include "read_stream.h"
#include "common.h"

// these constants can be generated with python - 'moov'[::-1].encode('hex')
#define ATOM_NAME_MOOV (0x766f6f6d)		// movie header
#define ATOM_NAME_TRAK (0x6b617274)		// track header
#define ATOM_NAME_MDIA (0x6169646d)		// media
#define ATOM_NAME_HDLR (0x726c6468)		// handler type
#define ATOM_NAME_MDHD (0x6468646d)		// media header
#define ATOM_NAME_MINF (0x666e696d)		// media information
#define ATOM_NAME_STBL (0x6c627473)		// sample table
#define ATOM_NAME_STCO (0x6f637473)		// sample table chunk offset
#define ATOM_NAME_CO64 (0x34366f63)		// sample table chunk offset 64 bit
#define ATOM_NAME_STSC (0x63737473)		// sample table sample to chunk map
#define ATOM_NAME_STSZ (0x7a737473)		// sample table sizes
#define ATOM_NAME_STZ2 (0x327a7473)		// sample table sizes
#define ATOM_NAME_STTS (0x73747473)		// sample table time to sample map
#define ATOM_NAME_CTTS (0x73747463)		// composition time to sample
#define ATOM_NAME_STSS (0x73737473)		// sample table sync samples
#define ATOM_NAME_STSD (0x64737473)		// sample table sample description
#define ATOM_NAME_AVCC (0x43637661)		// advanced video codec configuration
#define ATOM_NAME_ESDS (0x73647365)		// elementary stream description

#define ATOM_NAME_NULL (0x00000000)

// h264 4cc tags
#define FORMAT_AVC1	   (0x31637661)
#define FORMAT_h264	   (0x34363268)
#define FORMAT_H264	   (0x34363248)

// aac 4cc tag
#define FORMAT_MP4A    (0x6134706d)

#define HDLR_TYPE_VIDE (0x65646976)		// video track
#define HDLR_TYPE_SOUN (0x6e756f73)		// audio track

// MP4 constants from ffmpeg
#define MP4ODescrTag				0x01
#define MP4IODescrTag				0x02
#define MP4ESDescrTag				0x03
#define MP4DecConfigDescrTag		0x04
#define MP4DecSpecificDescrTag		0x05
#define MP4SLDescrTag				0x06

// mp4 atom structs
typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stco_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stsc_atom_t;

typedef struct {
	u_char	first_chunk[4];
	u_char	samples_per_chunk[4];
	u_char	sample_desc[4];
} stsc_entry_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	uniform_size[4];
	u_char	entries[4];
} stsz_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	reserved[3];
	u_char	field_size[1];
	u_char	entries[4];
} stz2_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stts_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} ctts_atom_t;

typedef struct {
	u_char	count[4];
	u_char	duration[4];
} stts_entry_t;

typedef struct {
	u_char	count[4];
	u_char	duration[4];
} ctts_entry_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stss_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stsd_atom_t;

typedef struct {
	u_char	size[4];
	u_char	format[4];
} stsd_entry_header_t;

typedef struct {
	u_char	reserved1[4];
	u_char	reserved2[2];
	u_char	dref_id[2];
} stsd_large_entry_header_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	creation_time[4];
	u_char	modification_time[4];
	u_char	timescale[4];
	u_char	duration[4];
	u_char	language[2];
	u_char	quality[2];
} mdhd_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	creation_time[8];
	u_char	modification_time[8];
	u_char	timescale[4];
	u_char	duration[8];
	u_char	language[2];
	u_char	quality[2];
} mdhd64_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	ctype[4];
	u_char	type[4];
	u_char	component_manufacture[4];
	u_char	component_flags[4];
	u_char	component_flags_mask[4];
} hdlr_atom_t;

typedef struct {
	u_char	version[2];
	u_char	revision_level[2];
	u_char	vendor[4];
	u_char	temporal_quality[4];
	u_char	spatial_quality[4];
	u_char	width[2];
	u_char	height[2];
	u_char	horiz_resolution[4];
	u_char	vert_resolution[4];
	u_char	data_size[4];
	u_char	frames_per_samples[2];
	u_char	codec_name[32];
	u_char	bits_per_coded_sample[2];
	u_char	colortable_id[2];
} stsd_video_t;

typedef struct {
	u_char	version[2];
	u_char	revision_level[2];
	u_char	vendor[4];
	u_char	channels[2];
	u_char	bits_per_coded_sample[2];
	u_char	audio_cid[2];
	u_char	packet_size[2];
	u_char	sample_rate[4];
} stsd_audio_t;

typedef struct {
	u_char	color_start[4];
	u_char	color_count[2];
	u_char	color_end[2];
} stsd_video_palette_header_t;

typedef struct {
	u_char	a[2];
	u_char	r[2];
	u_char	g[2];
	u_char	b[2];
} stsd_video_palette_entry_t;

// atom parsing types
typedef uint32_t atom_name_t;

typedef struct {
	atom_name_t atom_name;
	const u_char* ptr;
	uint64_t size;	
} atom_info_t;

typedef vod_status_t (*parse_atoms_callback_t)(void* context, atom_info_t* atom_info);

// relevant atoms finder
typedef struct relevant_atom_s {
	atom_name_t atom_name;
	int atom_info_offset;
	const struct relevant_atom_s* relevant_children;
} relevant_atom_t;

typedef struct {
	request_context_t* request_context;
	const relevant_atom_t* relevant_atoms;
	void* result;
} save_relevant_atoms_context_t;

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
} trak_atom_infos_t;

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
	{ ATOM_NAME_NULL, 0, NULL }
};

typedef struct {
	request_context_t* request_context;
	uint32_t timescale;
	uint32_t dts_shift;
	uint32_t first_frame;
	uint32_t last_frame;
	int media_type;
	uint32_t format;
	input_frame_t* frames_info;
	uint64_t* frame_offsets;
	uint32_t frame_count;
	u_char* extra_data;
	uint32_t extra_data_size;
	int64_t duration;
	uint32_t key_frame_count;
	uint32_t first_chunk_frame_index;
	bool_t chunk_equals_sample;
	uint64_t first_frame_chunk_offset;
} trak_info_t;

// implementation
static vod_status_t 
parse_atoms(request_context_t* request_context, const u_char* buffer, int buffer_size, bool_t validate_full_atom, parse_atoms_callback_t callback, void* context)
{
	const u_char* cur_pos = buffer;
	const u_char* end_pos = buffer + buffer_size;
	uint64_t atom_size;
	atom_info_t atom_info;
	unsigned atom_header_size;
	vod_status_t rc;
	
	while (cur_pos + 2 * sizeof(uint32_t) <= end_pos)
	{
		READ_BE32(cur_pos, atom_size);
		READ_LE32(cur_pos, atom_info.atom_name);
		
		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, 
			"parse_atoms: atom name=%*s, size=%uL", (size_t)sizeof(atom_info.atom_name), (char*)&atom_info.atom_name, atom_size);
		
		if (atom_size == 1)
		{
			// atom_size == 1 => atom uses 64 bit size
			if (cur_pos + sizeof(uint64_t) > end_pos)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"parse_atoms: atom size is 1 but there is not enough room for the 64 bit size");
				return VOD_BAD_DATA;
			}
			
			READ_BE64(cur_pos, atom_size);
			atom_header_size = 16;
		}
		else
		{
			atom_header_size = 8;
			if (atom_size == 0)
			{
				// atom_size == 0 => atom extends till the end of the buffer
				atom_size = (end_pos - cur_pos) + atom_header_size;
			}
		}
		
		if (atom_size < atom_header_size)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"parse_atoms: atom size %uL is less than the atom header size %ui", atom_size, atom_header_size);
			return VOD_BAD_DATA;
		}
		
		atom_size -= atom_header_size;
		if (validate_full_atom && atom_size > (uint64_t)(end_pos - cur_pos))
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"parse_atoms: atom size %uL overflows the input stream size %uL", atom_size, (uint64_t)(end_pos - cur_pos));
			return VOD_BAD_DATA;
		}
		
		atom_info.ptr = cur_pos;
		atom_info.size = atom_size;
		rc = callback(context, &atom_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
		
		if (atom_size > (uint64_t)(end_pos - cur_pos))
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"parse_atoms: atom size %uL overflows the input stream size %uL", atom_size, (uint64_t)(end_pos - cur_pos));
			return VOD_BAD_DATA;
		}
		cur_pos += atom_size;
	}
	
	return VOD_OK;
}

static vod_status_t 
find_atom_callback(void* ctx, atom_info_t* atom_info)
{
	atom_info_t* context = (atom_info_t*)ctx;
	
	if (atom_info->atom_name != context->atom_name)
	{
		return VOD_OK;
	}
	
	*context = *atom_info;
	
	return VOD_BAD_DATA;		// just to quit the loop, not really an error
}

static vod_status_t 
save_relevant_atoms_callback(void* ctx, atom_info_t* atom_info)
{
	save_relevant_atoms_context_t* context = (save_relevant_atoms_context_t*)ctx;
	save_relevant_atoms_context_t child_context;
	const relevant_atom_t* cur_atom;
	vod_status_t rc;
	
	for (cur_atom = context->relevant_atoms; cur_atom->atom_name != ATOM_NAME_NULL; cur_atom++)
	{
		if (cur_atom->atom_name != atom_info->atom_name)
		{
			continue;
		}
		
		if (cur_atom->relevant_children != NULL)
		{
			child_context.relevant_atoms = cur_atom->relevant_children;
			child_context.result = context->result;
			child_context.request_context = context->request_context;
			rc = parse_atoms(context->request_context, atom_info->ptr, atom_info->size, TRUE, &save_relevant_atoms_callback, &child_context);
			if (rc != VOD_OK)
			{
				return rc;
			}
			continue;
		}
		
		*(atom_info_t*)(((u_char*)context->result) + cur_atom->atom_info_offset) = *atom_info;
	}
	return VOD_OK;
}

static vod_status_t 
parse_hdlr_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const hdlr_atom_t* atom = (const hdlr_atom_t*)atom_info->ptr;
	uint32_t type;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_hdlr_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}
		
	type = PARSE_LE32(atom->type);
	switch (type)
	{
	case HDLR_TYPE_VIDE:
		trak_info->media_type = MEDIA_TYPE_VIDEO;
		break;

	case HDLR_TYPE_SOUN:
		trak_info->media_type = MEDIA_TYPE_AUDIO;
		break;
	}
	
	return VOD_OK;
}

static vod_status_t 
parse_mdhd_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const mdhd_atom_t* atom = (const mdhd_atom_t*)atom_info->ptr;
	const mdhd64_atom_t* atom64 = (const mdhd64_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_mdhd_atom: atom size %uL too small (1)", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_mdhd_atom: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}
			
		trak_info->timescale = PARSE_BE32(atom64->timescale);
		trak_info->duration = PARSE_BE64(atom64->duration);
	}
	else
	{
		trak_info->timescale = PARSE_BE32(atom->timescale);
		trak_info->duration = PARSE_BE32(atom->duration);
	}
	
	if (trak_info->timescale == 0)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_mdhd_atom: time scale is zero");
		return VOD_BAD_DATA;
	}
	
	return VOD_OK;
}

static vod_status_t 
parse_stts_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const stts_atom_t* atom = (const stts_atom_t*)atom_info->ptr;
	const stts_entry_t* last_entry;
	const stts_entry_t* cur_entry;
	uint32_t sample_count;
	uint32_t sample_duration;
	uint32_t entries;
	uint32_t start_time;
	uint32_t end_time;
	uint32_t accum_duration = 0;
	uint32_t next_accum_duration;
	uint32_t skip_count;
	uint32_t initial_alloc_size;
	input_frame_t* cur_frame;
	vod_array_t frames_array;
	uint32_t first_frame = UINT_MAX;
	uint32_t frame_index = 0;
	uint32_t frame_count = 0;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stts_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	entries = PARSE_BE32(atom->entries);
	if (entries >= (INT_MAX - sizeof(*atom)) / sizeof(*cur_entry))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stts_atom: number of entries %uD too big", entries);
		return VOD_BAD_DATA;
	}
	
	if (atom_info->size < sizeof(*atom) + entries * sizeof(*cur_entry))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stts_atom: atom size %uL too small to hold %uD entries", atom_info->size, entries);
		return VOD_BAD_DATA;
	}

	cur_entry = (const stts_entry_t*)(atom_info->ptr + sizeof(*atom));
	last_entry = cur_entry + entries;
	initial_alloc_size = 128;
	
	if (trak_info->request_context->start / 1000 + 1 > UINT_MAX / trak_info->timescale)			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stts_atom: start offset %uD too big", trak_info->request_context->start);
		return VOD_BAD_DATA;
	}

	start_time = (uint32_t)(((uint64_t)trak_info->request_context->start * trak_info->timescale) / 1000);
	if (trak_info->request_context->end == UINT_MAX)
	{
		end_time = UINT_MAX;

		// optimization - pre-allocate the correct size for constant frame rate
		if (entries == 1)
		{
			initial_alloc_size = PARSE_BE32(cur_entry->count);
		}
	}
	else
	{
		if (trak_info->request_context->end / 1000 + 1 > UINT_MAX / trak_info->timescale)			// integer overflow protection
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stts_atom: end offset %uD too big", trak_info->request_context->end);
			return VOD_BAD_DATA;
		}
	
		end_time = (uint32_t)(((uint64_t)trak_info->request_context->end * trak_info->timescale) / 1000);

		// optimization - pre-allocate the correct size for constant frame rate
		if (entries == 1)
		{
			sample_duration = PARSE_BE32(cur_entry->duration);
			if (sample_duration == 0)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stts_atom: sample duration is zero (1)");
				return VOD_BAD_DATA;
			}
			initial_alloc_size = (end_time - start_time) / sample_duration + 1;
		}
	}

	if (initial_alloc_size > trak_info->request_context->max_frame_count)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stts_atom: initial alloc size %uD exceeds the max frame count %uD", initial_alloc_size, trak_info->request_context->max_frame_count);
		return VOD_BAD_DATA;
	}

	if (vod_array_init(&frames_array, trak_info->request_context->pool, initial_alloc_size, sizeof(input_frame_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, trak_info->request_context->log, 0,
			"parse_stts_atom: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}
	
	for (; cur_entry < last_entry; cur_entry++)
	{
		if (accum_duration > end_time)
		{
			break;
		}
	
		sample_duration = PARSE_BE32(cur_entry->duration);
		if (sample_duration == 0)
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stts_atom: sample duration is zero (2)");
			return VOD_BAD_DATA;
		}
		
		sample_count = PARSE_BE32(cur_entry->count);
		next_accum_duration = accum_duration + sample_duration * sample_count;
		if (start_time > next_accum_duration)
		{
			frame_index += sample_count;
			accum_duration = next_accum_duration;
			continue;
		}
		
		if (start_time > accum_duration)
		{
			skip_count = (start_time - accum_duration) / sample_duration;
			sample_count -= skip_count;
			frame_index += skip_count;
			accum_duration += skip_count * sample_duration;
		}
		
		for (; sample_count != 0; sample_count--, frame_index++, accum_duration += sample_duration)
		{
			if (accum_duration < start_time || accum_duration >= end_time)
			{
				continue;
			}

			if (first_frame == UINT_MAX)
			{
				first_frame = frame_index;
			}
			
			if (frame_count > trak_info->request_context->max_frame_count)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stts_atom: frame count exceeds the limit %uD", trak_info->request_context->max_frame_count);
				return VOD_BAD_DATA;
			}
				
			cur_frame = vod_array_push(&frames_array);
			if (cur_frame == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, trak_info->request_context->log, 0,
					"parse_stts_atom: vod_array_push failed");
				return VOD_ALLOC_FAILED;
			}
			cur_frame->dts = accum_duration;
			cur_frame->pts = accum_duration;
			
			frame_count++;
		}
	}
	
	if (first_frame == UINT_MAX)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stts_atom: no frames were found within start %uD and end %uD", trak_info->request_context->start, trak_info->request_context->end);
		return VOD_BAD_REQUEST;
	}

	trak_info->frames_info = frames_array.elts;
	trak_info->first_frame = first_frame;
	trak_info->last_frame = first_frame + frame_count;
	trak_info->frame_count = frame_count;
	
	return VOD_OK;
}

static vod_status_t 
parse_ctts_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const ctts_atom_t* atom = (const ctts_atom_t*)atom_info->ptr;
	const ctts_entry_t* last_entry;
	const ctts_entry_t* cur_entry;
	uint32_t sample_count;
	uint32_t skip_count;
	int32_t sample_duration;
	uint32_t dts_shift = 0;
	uint32_t entries;
	uint32_t frame_index = 0;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}
	
	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_ctts_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	entries = PARSE_BE32(atom->entries);
	if (entries >= (INT_MAX - sizeof(*atom)) / sizeof(*cur_entry))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_ctts_atom: number of entries %uD too big", entries);
		return VOD_BAD_DATA;
	}
	
	if (atom_info->size < sizeof(*atom) + entries * sizeof(*cur_entry))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_ctts_atom: atom size %uL too small to hold %uD entries", atom_info->size, entries);
		return VOD_BAD_DATA;
	}

	cur_entry = (const ctts_entry_t*)(atom_info->ptr + sizeof(*atom));
	last_entry = cur_entry + entries;
	for (; cur_entry < last_entry; cur_entry++)
	{
		if (frame_index >= trak_info->last_frame)
		{
			break;
		}
	
		sample_duration = PARSE_BE32(cur_entry->duration);
		sample_count = PARSE_BE32(cur_entry->count);

		if (sample_duration < 0)
		{
			dts_shift = MAX(dts_shift, (uint32_t)-sample_duration);
		}
		
		if (trak_info->first_frame > frame_index + sample_count)
		{
			frame_index += sample_count;
			continue;
		}
		
		if (trak_info->first_frame > frame_index)
		{
			skip_count = trak_info->first_frame - frame_index;
			sample_count -= skip_count;
			frame_index += skip_count;			
		}

		for (; sample_count != 0; sample_count--, frame_index++)
		{
			if (frame_index < trak_info->first_frame || frame_index >= trak_info->last_frame)
			{
				continue;
			}
			
			trak_info->frames_info[frame_index - trak_info->first_frame].pts += sample_duration;
		}
	}
	
	trak_info->dts_shift = dts_shift;
	return VOD_OK;
}

static vod_status_t 
parse_stco_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const stco_atom_t* atom = (const stco_atom_t*)atom_info->ptr;
	input_frame_t* cur_frame = trak_info->frames_info;
	input_frame_t* last_frame = cur_frame + trak_info->frame_count;
	uint64_t* cur_offset;
	uint64_t* last_offset;
	uint32_t entries;
	const u_char* cur_pos;
	uint32_t entry_size;
	uint64_t cur_file_offset;
	uint32_t cur_chunk_index;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stco_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	// get and validate the number of entries
	entries = PARSE_BE32(atom->entries);

	if (atom_info->atom_name == ATOM_NAME_CO64)
	{
		entry_size = sizeof(uint64_t);
	}
	else
	{
		entry_size = sizeof(uint32_t);
	}

	if (entries >= (INT_MAX - sizeof(*atom)) / entry_size)			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stco_atom: number of entries %uD too big", entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + entries * entry_size)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stco_atom: atom size %uL too small to hold %uD entries", atom_info->size, entries);
		return VOD_BAD_DATA;
	}

	trak_info->frame_offsets = vod_alloc(trak_info->request_context->pool, trak_info->frame_count * sizeof(uint64_t));
	if (trak_info->frame_offsets == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, trak_info->request_context->log, 0,
			"parse_stco_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	cur_offset = trak_info->frame_offsets;
	last_offset = trak_info->frame_offsets + trak_info->frame_count;

	// optimization for the case in which chunk == sample
	if (trak_info->chunk_equals_sample)
	{
		if (entries < trak_info->last_frame)
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stco_atom: number of entries %uD smaller than last frame %uD", entries, trak_info->last_frame);
			return VOD_BAD_DATA;
		}

		cur_pos = atom_info->ptr + sizeof(*atom) + trak_info->first_frame * entry_size;
		if (atom_info->atom_name == ATOM_NAME_CO64)
		{
			for (; cur_offset < last_offset; cur_offset++)
			{
				READ_BE64(cur_pos, *cur_offset);
			}
		}
		else
		{
			for (; cur_offset < last_offset; cur_offset++)
			{
				READ_BE32(cur_pos, *cur_offset);
			}
		}
		return VOD_OK;
	}

	if (last_frame[-1].key_frame >= entries)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stco_atom: number of entries %uD smaller than last chunk %uD", entries, last_frame[-1].key_frame);
		return VOD_BAD_DATA;
	}

	cur_chunk_index = cur_frame->key_frame;			// Note: we use key_frame to store the chunk index since it's temporary
	cur_pos = atom_info->ptr + sizeof(*atom) + cur_chunk_index * entry_size;
	if (atom_info->atom_name == ATOM_NAME_CO64)
	{
		READ_BE64(cur_pos, cur_file_offset);
		cur_file_offset += trak_info->first_frame_chunk_offset;
		for (; cur_offset < last_offset; cur_offset++, cur_frame++)
		{
			if (cur_frame->key_frame != cur_chunk_index)
			{
				// Note: assuming chunk indexes always grow by 1, the way stsc is encoded ensures that's always true
				cur_chunk_index = cur_frame->key_frame;
				READ_BE64(cur_pos, cur_file_offset);
			}
			*cur_offset = cur_file_offset;
			cur_file_offset += cur_frame->size;
		}
	}
	else
	{
		READ_BE32(cur_pos, cur_file_offset);
		cur_file_offset += trak_info->first_frame_chunk_offset;
		for (; cur_offset < last_offset; cur_offset++, cur_frame++)
		{
			if (cur_frame->key_frame != cur_chunk_index)
			{
				// Note: assuming chunk indexes always grow by 1, the way stsc is encoded ensures that's always true
				cur_chunk_index = cur_frame->key_frame;
				READ_BE32(cur_pos, cur_file_offset);
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
parse_stsc_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const stsc_atom_t* atom = (const stsc_atom_t*)atom_info->ptr;
	input_frame_t* cur_frame = trak_info->frames_info;
	input_frame_t* last_frame = cur_frame + trak_info->frame_count;
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

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsc_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	entries = PARSE_BE32(atom->entries);
	if (entries >= (INT_MAX - sizeof(*atom)) / sizeof(stsc_entry_t))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsc_atom: number of entries %uD too big", entries);
		return VOD_BAD_DATA;
	}

	if (entries == 0 ||
		atom_info->size < sizeof(*atom) + entries * sizeof(stsc_entry_t))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsc_atom: atom size %uL too small to hold %uD entries", atom_info->size, entries);
		return VOD_BAD_DATA;
	}

	// optimization for the case where chunk == sample
	if (entries == 1 &&
		vod_memcmp(atom_info->ptr + sizeof(*atom), chunk_equals_sample_entry, sizeof(chunk_equals_sample_entry)) == 0)
	{
		trak_info->chunk_equals_sample = TRUE;
		trak_info->first_chunk_frame_index = frame_index;
		return VOD_OK;
	}

	cur_entry = (const stsc_entry_t*)(atom_info->ptr + sizeof(*atom));
	last_entry = cur_entry + entries;

	next_chunk = PARSE_BE32(cur_entry->first_chunk);
	if (next_chunk < 1)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsc_atom: chunk index is zero (1)");
		return VOD_BAD_DATA;
	}
	next_chunk--;		// convert to 0-based

	if (frame_index < trak_info->first_frame)
	{
		// skip to the relevant entry
		for (; cur_entry + 1 < last_entry; cur_entry++, frame_index += cur_entry_samples)
		{
			cur_chunk = next_chunk;
			next_chunk = PARSE_BE32(cur_entry[1].first_chunk);
			samples_per_chunk = PARSE_BE32(cur_entry->samples_per_chunk);
			if (samples_per_chunk == 0 || next_chunk < 1)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsc_atom: invalid samples per chunk %uD or chunk index %uD", samples_per_chunk, next_chunk);
				return VOD_BAD_DATA;
			}
			next_chunk--;			// convert to 0-based

			if (next_chunk <= cur_chunk)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsc_atom: chunk index %uD is smaller than the previous index %uD (1)", next_chunk, cur_chunk);
				return VOD_BAD_DATA;
			}

			if (next_chunk - cur_chunk > UINT_MAX / samples_per_chunk)		// integer overflow protection
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsc_atom: chunk index %uD is too big for previous index %uD and samples per chunk %uD", next_chunk, cur_chunk, samples_per_chunk);
				return VOD_BAD_DATA;
			}

			cur_entry_samples = (next_chunk - cur_chunk) * samples_per_chunk;
			if (cur_entry_samples > UINT_MAX - frame_index)		// integer overflow protection
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsc_atom: number of samples per entry %uD is too big", cur_entry_samples);
				return VOD_BAD_DATA;
			}

			if (frame_index + cur_entry_samples >= trak_info->first_frame)
			{
				next_chunk = cur_chunk;		// the first frame is within the current entry, revert the value of next chunk
				break;
			}
		}
	}

	for (; cur_entry < last_entry; cur_entry++)
	{
		// get and validate samples_per_chunk, cur_chunk and next_chunk
		samples_per_chunk = PARSE_BE32(cur_entry->samples_per_chunk);
		if (samples_per_chunk == 0)
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stsc_atom: samples per chunk is zero");
			return VOD_BAD_DATA;
		}
		cur_chunk = next_chunk;
		if (cur_entry + 1 < last_entry)
		{
			next_chunk = PARSE_BE32(cur_entry[1].first_chunk);
			if (next_chunk < 1)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsc_atom: chunk index is zero (2)");
				return VOD_BAD_DATA;
			}
			next_chunk--;			// convert to 0-based
			if (next_chunk <= cur_chunk)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsc_atom: chunk index %uD is smaller than the previous index %uD (2)", next_chunk, cur_chunk);
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
			if (frame_index < trak_info->first_frame)
			{
				// skip samples until we get to the first frame
				samples_to_skip = MIN(trak_info->first_frame - frame_index, samples_per_chunk);
				cur_sample -= samples_to_skip;
				frame_index += samples_to_skip;
			}

			for (; cur_sample; cur_sample--, frame_index++)
			{
				if (frame_index == trak_info->first_frame)
				{
					trak_info->first_chunk_frame_index = frame_index - (samples_per_chunk - cur_sample);
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
	vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
		"parse_stsc_atom: failed to get the chunk indexes for all frames");
	return VOD_BAD_DATA;
}

static vod_status_t 
parse_stsz_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const stsz_atom_t* atom = (const stsz_atom_t*)atom_info->ptr;
	const stz2_atom_t* atom2 = (const stz2_atom_t*)atom_info->ptr;
	input_frame_t* cur_frame = trak_info->frames_info;
	input_frame_t* last_frame = cur_frame + trak_info->frame_count;
	uint32_t first_frame_index_in_chunk = trak_info->first_frame - trak_info->first_chunk_frame_index;
	uint32_t entries;
	const u_char* cur_pos;
	uint32_t uniform_size;
	unsigned field_size;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsz_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom_info->atom_name == ATOM_NAME_STZ2)
	{
		field_size = atom2->field_size[0];
		if (field_size == 0)			// protect against division by zero
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stsz_atom: field size is zero");
			return VOD_BAD_DATA;
		}
	}
	else
	{
		uniform_size = PARSE_BE32(atom->uniform_size);
		if (uniform_size != 0)
		{
			if (uniform_size > MAX_FRAME_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsz_atom: uniform size %uD is too big", uniform_size);
				return VOD_BAD_DATA;
			}

			trak_info->first_frame_chunk_offset = ((uint64_t)first_frame_index_in_chunk) * uniform_size;
			for (; cur_frame < last_frame; cur_frame++)
			{
				cur_frame->size = uniform_size;
			}
			return VOD_OK;
		}
		field_size = 32;
	}
		
	entries = PARSE_BE32(atom->entries);
	if (entries < trak_info->last_frame)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsz_atom: number of entries %uD smaller than last frame %uD", entries, trak_info->last_frame);
		return VOD_BAD_DATA;
	}

	if (entries >= INT_MAX / field_size)			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsz_atom: number of entries %uD too big for size %ui bits", entries, field_size);
		return VOD_BAD_DATA;
	}
	
	if (atom_info->size < sizeof(*atom) + DIV_CEIL(entries * field_size, 8))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsz_atom: atom size %uL too small to hold %uD entries of %ui bits", atom_info->size, entries, field_size);
		return VOD_BAD_DATA;
	}
	
	switch (field_size)
	{
	case 32:
		cur_pos = atom_info->ptr + sizeof(*atom) + trak_info->first_chunk_frame_index * sizeof(uint32_t);
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--, cur_pos += sizeof(uint32_t))
		{
			trak_info->first_frame_chunk_offset += PARSE_BE32(cur_pos);
		}
		for (; cur_frame < last_frame; cur_frame++)
		{
			READ_BE32(cur_pos, cur_frame->size);
			if (cur_frame->size > MAX_FRAME_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
					"parse_stsz_atom: frame size %uD too big", cur_frame->size);
				return VOD_BAD_DATA;
			}
		}
		break;

	case 16:
		cur_pos = atom_info->ptr + sizeof(*atom) + trak_info->first_chunk_frame_index * sizeof(uint16_t);
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--, cur_pos += sizeof(uint16_t))
		{
			trak_info->first_frame_chunk_offset += PARSE_BE16(cur_pos);
		}
		for (; cur_frame < last_frame; cur_frame++)
		{
			READ_BE16(cur_pos, cur_frame->size);
			// Note: no need to validate the size here, since MAX_UINT16 < MAX_FRAME_SIZE
		}
		break;
		
	case 8:
		cur_pos = atom_info->ptr + sizeof(*atom) + trak_info->first_chunk_frame_index;
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--)
		{
			trak_info->first_frame_chunk_offset += *cur_pos++;
		}
		for (; cur_frame < last_frame; cur_frame++)
		{
			cur_frame->size = *cur_pos++;
			// Note: no need to validate the size here, since MAX_UINT8 < MAX_FRAME_SIZE
		}
		break;
		
	case 4:
		// TODO: implement this
		
	default:
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsz_atom: unsupported field size %ui", field_size);
		return VOD_BAD_DATA;
	}
	
	return VOD_OK;
}

static vod_status_t 
parse_stss_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const stss_atom_t* atom = (const stss_atom_t*)atom_info->ptr;
	input_frame_t* cur_frame = trak_info->frames_info;
	input_frame_t* last_frame = cur_frame + trak_info->frame_count;
	const u_char* cur_pos;
	const u_char* end_pos;
	uint32_t entries;
	uint32_t frame_index;

	for (; cur_frame < last_frame; cur_frame++)
	{
		cur_frame->key_frame = FALSE;
	}
	
	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}
	
	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stss_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	entries = PARSE_BE32(atom->entries);
	if (entries >= (INT_MAX - sizeof(*atom)) / sizeof(uint32_t))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stss_atom: number of entries %uD too big ", entries);
		return VOD_BAD_DATA;
	}
		
	if (atom_info->size < sizeof(*atom) + entries * sizeof(uint32_t))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stss_atom: atom size %uL too small to hold %uD entries", atom_info->size, entries);
		return VOD_BAD_DATA;
	}
		
	cur_pos = atom_info->ptr + sizeof(*atom);
	end_pos = cur_pos + entries * sizeof(uint32_t);
	for (; cur_pos < end_pos; cur_pos += sizeof(uint32_t))
	{
		frame_index = PARSE_BE32(cur_pos) - 1;		// 1 based index
		if (frame_index >= trak_info->first_frame && frame_index < trak_info->last_frame)
		{
			cur_frame = &trak_info->frames_info[frame_index - trak_info->first_frame];
			if (!cur_frame->key_frame)		// increment only once in case a frame is listed twice
			{
				cur_frame->key_frame = TRUE;
				trak_info->key_frame_count++;
			}
		}
	}
	
	return VOD_OK;
}

static vod_status_t 
parse_video_extra_data_atom(void* context, atom_info_t* atom_info)
{
	trak_info_t* trak_info = (trak_info_t*)context;
	
	if (atom_info->atom_name != ATOM_NAME_AVCC)
	{
		return VOD_OK;
	}
	
	trak_info->extra_data_size = atom_info->size;
	trak_info->extra_data = vod_alloc(trak_info->request_context->pool, trak_info->extra_data_size);
	if (trak_info->extra_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, trak_info->request_context->log, 0,
			"parse_video_extra_data_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	
	vod_memcpy(trak_info->extra_data, atom_info->ptr, trak_info->extra_data_size);
	
	return VOD_OK;
}

// Note: no validations in the functions below - the stream functions will make sure 
//		we don't overflow the input buffer
static int 
ff_mp4_read_descr_len(simple_read_stream_t* stream)
{
	int len = 0;
	int count = 4;
	while (count--) 
	{
		int c = stream_get8(stream);
		len = (len << 7) | (c & 0x7f);
		if (!(c & 0x80))
		{
			break;
		}
	}
	return len;
}

static int 
ff_mp4_read_descr(simple_read_stream_t* stream, int *tag)
{
	*tag = stream_get8(stream);
	return ff_mp4_read_descr_len(stream);
}

static void 
ff_mp4_parse_es_descr(simple_read_stream_t* stream)
{
	int flags;
	
	stream_skip(stream, 2);
	flags = stream_get8(stream);
	if (flags & 0x80) //streamDependenceFlag
	{
		stream_skip(stream, 2);
	}
	if (flags & 0x40) //URL_Flag
	{
		int len = stream_get8(stream);
		stream_skip(stream, len);
	}
	if (flags & 0x20) //OCRstreamFlag
	{
		stream_skip(stream, 2);
	}
}

typedef struct {
	u_char object_type_id[1];
	u_char stream_type[1];
	u_char buffer_size[3];
	u_char max_bitrate[4];
	u_char avg_bitrate[4];
} config_descr_t;

static vod_status_t 
ff_mp4_read_dec_config_descr(trak_info_t* trak_info, simple_read_stream_t* stream)
{
	unsigned len;
	int tag;

	stream_skip(stream, sizeof(config_descr_t));

	len = ff_mp4_read_descr(stream, &tag);
	if (tag == MP4DecSpecificDescrTag)
	{
		if (len > stream->end_pos - stream->cur_pos)
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"ff_mp4_read_dec_config_descr: tag length %ui too big", len);
			return VOD_BAD_DATA;
		}
		trak_info->extra_data_size = len;
		trak_info->extra_data = vod_alloc(trak_info->request_context->pool, trak_info->extra_data_size);
		if (trak_info->extra_data == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, trak_info->request_context->log, 0,
				"ff_mp4_read_dec_config_descr: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
		
		vod_memcpy(trak_info->extra_data, stream->cur_pos, trak_info->extra_data_size);		
	}
	
	return VOD_OK;
}

static vod_status_t 
parse_audio_extra_data_atom(void* context, atom_info_t* atom_info)
{
	trak_info_t* trak_info = (trak_info_t*)context;
	simple_read_stream_t stream;
	int tag;
	vod_status_t rc;
	
	if (atom_info->atom_name != ATOM_NAME_ESDS)
	{
		return VOD_OK;
	}
	
	stream.cur_pos = atom_info->ptr;
	stream.end_pos = stream.cur_pos + atom_info->size;
	
	stream_skip(&stream, 4);		// version + flags
	ff_mp4_read_descr(&stream, &tag);
	if (tag == MP4ESDescrTag) 
	{
		ff_mp4_parse_es_descr(&stream);
	}
	else
	{
		stream_skip(&stream, 2);		// ID
	}

	ff_mp4_read_descr(&stream, &tag);
	if (tag == MP4DecConfigDescrTag)
	{
		rc = ff_mp4_read_dec_config_descr(trak_info, &stream);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return VOD_OK;
}

static const u_char* 
skip_stsd_atom_video(const u_char* cur_pos, const u_char* end_pos)
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
	
	bits_per_coded_sample = PARSE_BE16(video->bits_per_coded_sample);
	colortable_id = PARSE_BE16(video->colortable_id);		
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
			
			color_start = PARSE_BE32(palette->color_start);
			color_end = PARSE_BE16(palette->color_end);
			if ((color_start <= 255) && (color_end <= 255) && color_end >= color_start) 
			{
				cur_pos += (color_end - color_start + 1) * sizeof(stsd_video_palette_entry_t);
			}
		}
	}
	
	return cur_pos;
}

static const u_char* 
skip_stsd_atom_audio(const u_char* cur_pos, const u_char* end_pos)
{
	if (cur_pos + sizeof(stsd_audio_t) > end_pos)
	{
		return NULL;
	}
		
	cur_pos += sizeof(stsd_audio_t);
	
	// TODO: support QT version 1/2 additional fields	
	return cur_pos;
}

static vod_status_t 
parse_stsd_atom(atom_info_t* atom_info, trak_info_t* trak_info)
{
	const stsd_atom_t* atom = (const stsd_atom_t*)atom_info->ptr;
	uint32_t entries;
	const u_char* cur_pos;
	const u_char* end_pos;
	uint32_t size;
	const u_char* (*skip_function)(const u_char* cur_pos, const u_char* end_pos);
	parse_atoms_callback_t parse_function;
	vod_status_t rc;

	switch (trak_info->media_type)
	{
	case MEDIA_TYPE_VIDEO:
		skip_function = skip_stsd_atom_video;
		parse_function = parse_video_extra_data_atom;
		break;

	case MEDIA_TYPE_AUDIO:
		skip_function = skip_stsd_atom_audio;
		parse_function = parse_audio_extra_data_atom;		
		break;
		
	default:
		return VOD_OK;
	}
	
	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsd_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	cur_pos = atom_info->ptr + sizeof(*atom);
	end_pos = atom_info->ptr + atom_info->size;
	for (entries = PARSE_BE32(atom->entries); entries; entries--)
	{
		if (cur_pos + sizeof(stsd_entry_header_t) > end_pos)
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stsd_atom: not enough room for stsd entry");
			return VOD_BAD_DATA;
		}
	
		trak_info->format = PARSE_LE32(((stsd_entry_header_t*)cur_pos)->format);
		size = PARSE_BE32(((stsd_entry_header_t*)cur_pos)->size);
		cur_pos += sizeof(stsd_entry_header_t);
		if (size >= 16)
		{
			cur_pos += sizeof(stsd_large_entry_header_t);
		}
		
		cur_pos = skip_function(cur_pos, end_pos);
		if (cur_pos == NULL)
		{
			vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
				"parse_stsd_atom: failed to skip audio/video data");
			return VOD_BAD_DATA;
		}
	}

	if (cur_pos > end_pos)
	{
		vod_log_error(VOD_LOG_ERR, trak_info->request_context->log, 0,
			"parse_stsd_atom: stream overflow before reaching extra data");
		return VOD_BAD_DATA;
	}
	
	if (cur_pos < end_pos)
	{
		vod_log_buffer(VOD_LOG_DEBUG_LEVEL, trak_info->request_context->log, 0, "parse_stsd_atom: extra data ", cur_pos, end_pos - cur_pos);
		
		rc = parse_atoms(trak_info->request_context, cur_pos, end_pos - cur_pos, TRUE, parse_function, trak_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return VOD_OK;
}

typedef struct {
	vod_status_t (*parse)(atom_info_t* atom_info, trak_info_t* trak_info);
	int offset;
} trak_atom_parser_t;

static const trak_atom_parser_t trak_atom_parsers_segment[] = {
	// order is important
	{ parse_mdhd_atom, offsetof(trak_atom_infos_t, mdhd), },
	{ parse_stts_atom, offsetof(trak_atom_infos_t, stts), },
	{ parse_ctts_atom, offsetof(trak_atom_infos_t, ctts), },
	{ parse_stsc_atom, offsetof(trak_atom_infos_t, stsc), },
	{ parse_stsz_atom, offsetof(trak_atom_infos_t, stsz), },
	{ parse_stco_atom, offsetof(trak_atom_infos_t, stco), },
	{ parse_stss_atom, offsetof(trak_atom_infos_t, stss), },
	{ NULL, 0 }
};

static const trak_atom_parser_t trak_atom_parsers_iframes[] = {
	// order is important
	{ parse_mdhd_atom, offsetof(trak_atom_infos_t, mdhd), },
	{ parse_stts_atom, offsetof(trak_atom_infos_t, stts), },
	{ parse_ctts_atom, offsetof(trak_atom_infos_t, ctts), },
	{ parse_stsz_atom, offsetof(trak_atom_infos_t, stsz), },
	{ parse_stss_atom, offsetof(trak_atom_infos_t, stss), },
	{ NULL, 0 }
};

static const trak_atom_parser_t trak_atom_parsers_index[] = {
	{ parse_mdhd_atom, offsetof(trak_atom_infos_t, mdhd), },
	{ NULL, 0 }
};

typedef struct {
	request_context_t* request_context;
	mpeg_metadata_t* result;
	uint32_t* required_tracks_mask;
	uint32_t track_indexes[MEDIA_TYPE_COUNT];
	const trak_atom_parser_t* parsers;
} process_moov_context_t;

static vod_status_t 
process_moov_atom_callback(void* ctx, atom_info_t* atom_info)
{
	process_moov_context_t* context = (process_moov_context_t*)ctx;
	save_relevant_atoms_context_t save_atoms_context;
	trak_atom_infos_t trak_atom_infos;
	trak_info_t trak_info;
	mpeg_metadata_t* result = context->result;
	mpeg_stream_metadata_t* result_stream;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t track_index;
	bool_t format_supported = FALSE;
	const trak_atom_parser_t* cur_parser;
	vod_status_t rc;
#if (VOD_DEBUG)
	int parser_index = 0;
#endif

	if (atom_info->atom_name != ATOM_NAME_TRAK)
	{
		return VOD_OK;
	}
	
	// find required trak atoms
	vod_memzero(&trak_atom_infos, sizeof(trak_atom_infos));
	save_atoms_context.relevant_atoms = relevant_atoms_trak;
	save_atoms_context.result = &trak_atom_infos;
	save_atoms_context.request_context = context->request_context;
	rc = parse_atoms(context->request_context, atom_info->ptr, atom_info->size, TRUE, &save_relevant_atoms_callback, &save_atoms_context);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	// get the media info
	vod_memzero(&trak_info, sizeof(trak_info));
	trak_info.request_context = context->request_context;
	rc = parse_hdlr_atom(&trak_atom_infos.hdlr, &trak_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the codec type and extra data
	rc = parse_stsd_atom(&trak_atom_infos.stsd, &trak_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// make sure the codec is supported
	switch (trak_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		switch (trak_info.format)
		{
		case FORMAT_AVC1:
		case FORMAT_h264:
		case FORMAT_H264:
			format_supported = TRUE;
			break;
		}
		break;

	case MEDIA_TYPE_AUDIO:
		format_supported = (trak_info.format == FORMAT_MP4A);
		break;
	}

	if (!format_supported)
	{
		vod_free(context->request_context->pool, trak_info.extra_data);
		return VOD_OK;
	}

	// check whether we should include this track
	track_index = context->track_indexes[trak_info.media_type]++;
	if ((context->required_tracks_mask[trak_info.media_type] & (1 << track_index)) == 0)
	{
		vod_free(context->request_context->pool, trak_info.extra_data);
		return VOD_OK;
	}

	// parse the rest of the trak atoms
	for (cur_parser = context->parsers; cur_parser->parse; cur_parser++)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0, "running parser %i", parser_index++);
		rc = cur_parser->parse((atom_info_t*)((u_char*)&trak_atom_infos + cur_parser->offset), &trak_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// make sure we got the extra data
	if (trak_info.extra_data == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"process_moov_atom_callback: no extra data was parsed for track");
		return VOD_BAD_DATA;
	}

	result_stream = vod_array_push(&result->streams);
	if (result_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"process_moov_atom_callback: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}
		
	// copy the result
	result_stream->media_type = trak_info.media_type;
	result_stream->track_index = track_index;
	result_stream->frames = trak_info.frames_info;
	result_stream->frame_offsets = trak_info.frame_offsets;
	result_stream->frame_count = trak_info.frame_count;
	result_stream->extra_data = trak_info.extra_data;
	result_stream->extra_data_size = trak_info.extra_data_size;
	result_stream->duration = (trak_info.duration * 90000) / trak_info.timescale;
	result_stream->key_frame_count = trak_info.key_frame_count;

	// normalize the frame timestamps to 90KHz
	cur_frame = trak_info.frames_info;
	last_frame = cur_frame + trak_info.frame_count;
	for (; cur_frame < last_frame; cur_frame++)
	{
		cur_frame->pts = ((cur_frame->pts + trak_info.dts_shift) * 90000) / trak_info.timescale;
		cur_frame->dts = (cur_frame->dts * 90000) / trak_info.timescale;
	}

	return VOD_OK;
}

vod_status_t 
get_moov_atom_info(request_context_t* request_context, const u_char* buffer, size_t buffer_size, off_t* offset, size_t* size)
{
	atom_info_t find_moov_context = { ATOM_NAME_MOOV, NULL, 0 };

	// not checking the result of parse_atoms since we always stop the enumeration here
	parse_atoms(request_context, buffer, buffer_size, FALSE, &find_atom_callback, &find_moov_context);
	if (find_moov_context.ptr == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"get_moov_atom_info: failed to find moov atom start (file is not fast-start ?)");
		return VOD_BAD_DATA;
	}
		
	*offset = find_moov_context.ptr - buffer;
	*size = find_moov_context.size;
	
	return VOD_OK;
}

static const trak_atom_parser_t* parsers[] = {
	trak_atom_parsers_segment,
	trak_atom_parsers_iframes,
	trak_atom_parsers_index,
};

vod_status_t 
mp4_parser_parse_moov_atom(
	request_context_t* request_context, 
	int parse_type,
	uint32_t* required_tracks_mask,
	const u_char* buffer, 
	size_t size, 
	mpeg_metadata_t* mpeg_metadata)
{
	process_moov_context_t process_moov_context;
	vod_status_t rc;

	process_moov_context.request_context = request_context;
	process_moov_context.required_tracks_mask = required_tracks_mask;
	vod_memzero(process_moov_context.track_indexes, sizeof(process_moov_context.track_indexes));
	process_moov_context.result = mpeg_metadata;
	process_moov_context.parsers = parsers[parse_type];

	if (vod_array_init(&mpeg_metadata->streams, request_context->pool, 2, sizeof(mpeg_stream_metadata_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_parse_moov_atom: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	rc = parse_atoms(request_context, buffer, size, TRUE, &process_moov_atom_callback, &process_moov_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
