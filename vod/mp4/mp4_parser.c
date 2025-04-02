#include "mp4_parser.h"
#include "mp4_format.h"
#include "mp4_defs.h"
#include "../media_format.h"
#include "../input/frames_source_cache.h"
#include "../read_stream.h"
#include "../write_stream.h"
#include "../codec_config.h"
#include "../media_clip.h"
#include "../segmenter.h"
#include "../common.h"

#include <limits.h>

#if (VOD_HAVE_ZLIB)
#include <zlib.h>
#endif // VOD_HAVE_ZLIB

#if (VOD_HAVE_OPENSSL_EVP)
#include "mp4_cenc_decrypt.h"
#include "mp4_aes_ctr.h"
#endif // VOD_HAVE_OPENSSL_EVP

// TODO: use iterators from mp4_parser_base.c to reduce code duplication

// macros
#define member_size(type, member) sizeof(((type *)0)->member)

#define set_raw_atom(target, source)							\
	{															\
	(target).ptr = (source).ptr - (source).header_size;		\
	(target).size = (source).size + (source).header_size;	\
	}

#define write_swap16(p, s)		\
	{							\
	*(p)++ = (s)[1];			\
	*(p)++ = (s)[0];			\
	}

#define write_swap32(p, s)		\
	{							\
	*(p)++ = (s)[3];			\
	*(p)++ = (s)[2];			\
	*(p)++ = (s)[1];			\
	*(p)++ = (s)[0];			\
	}

// constants
#define MAX_FRAMERATE_TEST_SAMPLES (20)
#define MAX_TOTAL_SIZE_TEST_SAMPLES (100000)
#define MAX_PTS_DELAY_TEST_SAMPLES (100)
#define MAX_KEY_FRAME_BITRATE_TEST_SAMPLES (1000)

#define OPUS_EXTRA_DATA_MAGIC "OpusHead"

// typedefs
typedef struct {
	media_base_metadata_t base;		// tracks array is of mp4_track_base_metadata_t
	uint32_t mvhd_timescale;
} mp4_base_metadata_t;

// trak atom parsing
typedef struct {
	atom_info_t stco;
	atom_info_t stsc;
	atom_info_t stsz;
	atom_info_t stts;
	atom_info_t ctts;
	atom_info_t stss;
	atom_info_t stsd;
	atom_info_t saiz;
	atom_info_t senc;
	atom_info_t hdlr;
	atom_info_t mdhd;
	atom_info_t dinf;
	atom_info_t elst;
	atom_info_t tkhd;
	atom_info_t udta_name;
} trak_atom_infos_t;

typedef struct {
	request_context_t* request_context;
	media_parse_params_t parse_params;
	uint32_t track_indexes[MEDIA_TYPE_COUNT];
	vod_str_t ftyp_atom;
	mp4_base_metadata_t* result;
} process_moov_context_t;

typedef struct {
	request_context_t* request_context;
	media_parse_params_t parse_params;
	media_info_t media_info;
	atom_info_t sinf_atom;
} metadata_parse_context_t;

typedef struct {
	// input - consistent across tracks
	request_context_t* request_context;
	media_info_t* media_info;
	media_parse_params_t parse_params;
	uint64_t clip_from;
	uint32_t mvhd_timescale;

	// input - reset between tracks
	const uint32_t* stss_start_pos;			// initialized only when aligning keyframes
	uint32_t stss_entries;					// initialized only when aligning keyframes

	// output
	uint32_t stss_start_index;
	uint32_t dts_shift;
	uint32_t first_frame;
	uint32_t last_frame;
	uint32_t clip_to;
	uint64_t first_frame_time_offset;
	int32_t clip_from_frame_offset;
	input_frame_t* frames;
	uint32_t frame_count;
	uint64_t total_frames_size;
	uint64_t total_frames_duration;
	uint32_t key_frame_count;
	uint32_t first_chunk_frame_index;
	bool_t chunk_equals_sample;
	uint64_t first_frame_chunk_offset;
	media_encryption_t encryption_info;
	uint32_t auxiliary_info_start_offset;
	uint32_t auxiliary_info_end_offset;
} frames_parse_context_t;

typedef struct {
	trak_atom_infos_t trak_atom_infos;
	media_info_t media_info;
	atom_info_t sinf_atom;
	uint32_t track_index;
} mp4_track_base_metadata_t;

typedef struct {
	vod_status_t(*parse)(atom_info_t* atom_info, frames_parse_context_t* context);
	int offset;
	uint32_t flag;
} trak_atom_parser_t;

typedef struct {
	u_char magic[8];
	u_char version[1];
	u_char channels[1];
	u_char initial_padding[2];
	u_char sample_rate[4];
	u_char gain[2];
	u_char mapping_family[1];
} opus_extra_data_t;

typedef struct {
	u_char version[1];
	u_char channels[1];
	u_char initial_padding[2];
	u_char sample_rate[4];
	u_char gain[2];
	u_char mapping_family[1];
} dops_atom_t;

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
	{ ATOM_NAME_SAIZ, offsetof(trak_atom_infos_t, saiz), NULL },
	{ ATOM_NAME_SENC, offsetof(trak_atom_infos_t, senc), NULL },		// senc should be under trak, maintained for backward compatibility
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

static const relevant_atom_t relevant_atoms_edts[] = {
	{ ATOM_NAME_ELST, offsetof(trak_atom_infos_t, elst), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_udta[] = {
	{ ATOM_NAME_NAME, offsetof(trak_atom_infos_t, udta_name), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_trak[] = {
	{ ATOM_NAME_MDIA, 0, relevant_atoms_mdia },
	{ ATOM_NAME_EDTS, 0, relevant_atoms_edts },
	{ ATOM_NAME_TKHD, offsetof(trak_atom_infos_t, tkhd), NULL },
	{ ATOM_NAME_SENC, offsetof(trak_atom_infos_t, senc), NULL },
	{ ATOM_NAME_UDTA, 0, relevant_atoms_udta },
	{ ATOM_NAME_NULL, 0, NULL }
};

typedef struct {
	int raw_atom_index;
	int atom_info_offset;
} raw_atom_mapping_t;

static const raw_atom_mapping_t raw_atom_mapping[] = {
	{ RTA_STSD, offsetof(trak_atom_infos_t, stsd) },
};

// compressed moov
typedef struct {
	atom_info_t dcom;
	atom_info_t cmvd;
} moov_atom_infos_t;

static const relevant_atom_t relevant_atoms_cmov[] = {
	{ ATOM_NAME_DCOM, offsetof(moov_atom_infos_t, dcom), NULL },
	{ ATOM_NAME_CMVD, offsetof(moov_atom_infos_t, cmvd), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_moov[] = {
	{ ATOM_NAME_CMOV, 0, relevant_atoms_cmov },
	{ ATOM_NAME_NULL, 0, NULL }
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
	media_tags_t* tags;
	vod_str_t name;
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
	
	// parse the name / already set
	tags = &context->media_info.tags;
	if ((context->parse_params.parse_type & PARSE_FLAG_HDLR_NAME) == 0 || tags->label.data != NULL)
	{
		return VOD_OK;
	}

	name.data = (u_char*)(atom + 1);
	name.len = atom_info->ptr + atom_info->size - name.data;
	if (name.len > 0 && name.data[0] == name.len - 1)
	{
		name.data++;
		name.len--;
	}

	while (name.len > 0 && name.data[name.len - 1] == '\0')
	{
		name.len--;
	}

	if (name.len <= 0)
	{
		return VOD_OK;
	}

	tags->label.data = vod_alloc(context->request_context->pool, name.len + 1);
	if (tags->label.data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_hdlr_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memcpy(tags->label.data, name.data, name.len);
	tags->label.data[name.len] = '\0';
	tags->label.len = name.len;

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
mp4_parser_parse_elst_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const elst_atom_t* atom = (const elst_atom_t*)atom_info->ptr;
	const elst_entry_t* entry = NULL;
	const elst64_entry_t* entry64 = NULL;
	uint32_t entries;
	uint32_t entry_size;
	int64_t time;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_elst_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	entries = parse_be32(atom->entries);
	if (entries <= 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_elst_atom: zero entries");
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		entry64 = (const elst64_entry_t*)(atom_info->ptr + sizeof(*atom));
		entry_size = sizeof(*entry64);
	}
	else
	{
		entry = (const elst_entry_t*)(atom_info->ptr + sizeof(*atom));
		entry_size = sizeof(*entry);
	}

	if (entries >= (INT_MAX - sizeof(*atom)) / entry_size)			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_elst_atom: number of entries %uD too big", entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + entries * entry_size)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_elst_atom: atom size %uL too small to hold %uD entries", atom_info->size, entries);
		return VOD_BAD_DATA;
	}

	if (entries > 2)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_elst_atom: unsupported edit - atom has %uD entries", entries);
	}

	if (atom->version[0] == 1)
	{
		time = parse_be64(entry64[0].time);
	}
	else
	{
		time = (int32_t)parse_be32(entry[0].time);		// the cast is needed to sign extend negatives
	}

	if (time == -1)
	{
		if (atom->version[0] == 1)
		{
			context->media_info.empty_duration = parse_be64(entry64[0].duration);
		}
		else
		{
			context->media_info.empty_duration = (int32_t)parse_be32(entry[0].duration);
		}

		if (entries > 1)
		{
			if (atom->version[0] == 1)
			{
				context->media_info.start_time = parse_be64(entry64[1].time);
			}
			else
			{
				context->media_info.start_time = (int32_t)parse_be32(entry[1].time);
			}
		}
	}
	else if (time >= 0)
	{
		if (entries == 2)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_elst_atom: unsupported edit - atom has 2 entries and the first is not empty");
		}
		context->media_info.start_time = time;
	}
	else
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_elst_atom: unsupported edit - time is %L", time);
	}

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_mdhd_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const mdhd_atom_t* atom = (const mdhd_atom_t*)atom_info->ptr;
	const mdhd64_atom_t* atom64 = (const mdhd64_atom_t*)atom_info->ptr;
	media_tags_t* tags;
	uint64_t duration;
	uint32_t timescale;
	uint16_t language;

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
			
		timescale = parse_be32(atom64->timescale);
		duration = parse_be64(atom64->duration);
		language = parse_be16(atom64->language);
	}
	else
	{
		timescale = parse_be32(atom->timescale);
		duration = parse_be32(atom->duration);
		language = parse_be16(atom->language);
	}
	
	if (timescale == 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_mdhd_atom: timescale is zero");
		return VOD_BAD_DATA;
	}

	if (duration > (uint64_t)MAX_DURATION_SEC * timescale)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_mdhd_atom: media duration %uL too big", duration);
		return VOD_BAD_DATA;
	}

	context->media_info.timescale = timescale;
	context->media_info.frames_timescale = timescale;
	context->media_info.full_duration = duration;
	context->media_info.duration_millis = rescale_time(duration, timescale, 1000);

	tags = &context->media_info.tags;
	tags->language = lang_parse_iso639_3_code(language);
	if (tags->language != 0)
	{
		tags->lang_str.data = (u_char *)lang_get_rfc_5646_name(tags->language);
		tags->lang_str.len = ngx_strlen(tags->lang_str.data);

		if (tags->label.len == 0)
		{
			lang_get_native_name(tags->language, &tags->label);
		}
	}

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_udta_name_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	media_tags_t* tags;
	vod_str_t name;

	name.data = (u_char*)atom_info->ptr;
	name.len = atom_info->size;

	// atom empty/non-existent or name already set
	tags = &context->media_info.tags;
	if (name.len == 0 || tags->label.data != NULL)
	{
		return VOD_OK;
	}

	tags->label.data = vod_alloc(
		context->request_context->pool,
		name.len + 1);
	if (tags->label.data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_udta_name_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memcpy(tags->label.data, name.data, name.len);
	tags->label.data[name.len] = '\0';
	tags->label.len = name.len;

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_stts_atom_total_duration_only(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	const stts_entry_t* last_entry;
	const stts_entry_t* cur_entry;
	uint64_t duration;
	uint32_t timescale;
	uint32_t entries;
	uint32_t sample_duration;
	uint32_t sample_count;
	vod_status_t rc;

	rc = mp4_parser_validate_stts_data(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	cur_entry = (const stts_entry_t*)(atom_info->ptr + sizeof(stts_atom_t));
	last_entry = cur_entry + entries;

	duration = 0;
	for (; cur_entry < last_entry; cur_entry++)
	{
		sample_duration = parse_be32(cur_entry->duration);
		sample_count = parse_be32(cur_entry->count);
		duration += (uint64_t)sample_duration * sample_count;
	}

	timescale = context->media_info.timescale;
	if (duration > (uint64_t)MAX_DURATION_SEC * timescale)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stts_atom_total_duration_only: media duration %uL too big", duration);
		return VOD_BAD_DATA;
	}

	context->media_info.full_duration = duration;
	context->media_info.duration_millis = rescale_time(duration, timescale, 1000);

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
		cur_duration = parse_be32(cur_entry->duration);
		if (cur_duration == 0)
		{
			continue;
		}

		if (cur_duration != 0 && (media_info->min_frame_duration == 0 || cur_duration < media_info->min_frame_duration))
		{
			media_info->min_frame_duration = cur_duration;
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
	media_range_t* range = context->parse_params.range;
	uint32_t sample_count;
	uint32_t sample_duration;
	uint32_t entries;
	uint64_t clip_from;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t clip_to;
	uint64_t clip_from_accum_duration = 0;
	uint64_t accum_duration;
	uint64_t next_accum_duration;
	int64_t empty_duration;
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
	
	// calculate the initial duration for this stream
	empty_duration = rescale_time_neg(context->media_info->empty_duration, context->mvhd_timescale, timescale);
	if (empty_duration > context->media_info->start_time)
	{
		accum_duration = empty_duration - context->media_info->start_time;
	}
	else
	{
		// TODO: support negative offsets
		accum_duration = 0;
	}

	// parse the first sample
	cur_entry = (const stts_entry_t*)(atom_info->ptr + sizeof(stts_atom_t));
	last_entry = cur_entry + entries;
	if (cur_entry >= last_entry)
	{
		if (context->stss_entries != 0)
		{
			range->start = 0;
			range->end = 0;
		}
		return VOD_OK;
	}

	sample_duration = parse_be32(cur_entry->duration);
	sample_count = parse_be32(cur_entry->count);
	next_accum_duration = accum_duration + (uint64_t)sample_duration * sample_count;

	if (context->parse_params.clip_from > 0)
	{
		clip_from = (((uint64_t)context->parse_params.clip_from * timescale) / 1000);

		for (;;)
		{
			if (clip_from + sample_duration <= next_accum_duration)
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
					range->start = 0;
					range->end = 0;
				}
				return VOD_OK;
			}

			sample_duration = parse_be32(cur_entry->duration);
			sample_count = parse_be32(cur_entry->count);
			next_accum_duration = accum_duration + (uint64_t)sample_duration * sample_count;
		}

		if (clip_from > accum_duration)
		{
			skip_count = vod_div_ceil(clip_from - accum_duration, sample_duration);
			sample_count -= skip_count;
			frame_index += skip_count;
			accum_duration += (uint64_t)skip_count * sample_duration;
		}

		if (context->stss_entries != 0)
		{
			// jump to the first key frame after the clip position
			key_frame_stss_index = mp4_parser_find_stss_entry(frame_index, context->stss_start_pos, context->stss_entries);
			if (key_frame_stss_index >= context->stss_entries)
			{
				// can't find any key frame after the clip position
				range->start = 0;
				range->end = 0;
				return VOD_OK;
			}

			stss_entry = context->stss_start_pos + key_frame_stss_index;
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

				sample_duration = parse_be32(cur_entry->duration);
				sample_count = parse_be32(cur_entry->count);
				next_accum_duration = accum_duration + (uint64_t)sample_duration * sample_count;
			}

			// skip to the key frame within the current entry
			skip_count = key_frame_index - frame_index;
			sample_count -= skip_count;
			frame_index = key_frame_index;
			accum_duration += (uint64_t)skip_count * sample_duration;

			// update the parse params clip from so that subsequent tracks will
			// have their timestamps start from the same position
			context->parse_params.clip_from = (accum_duration * 1000) / timescale;
			clip_from = (((uint64_t)context->parse_params.clip_from * timescale) / 1000);
		}

		// calculate the clip from duration
		clip_from_accum_duration = accum_duration;

		context->clip_from_frame_offset = clip_from_accum_duration - clip_from;
	}
	else
	{
		clip_from = 0;
	}

	// skip to the sample containing the start time
	start_time = ((range->start + context->clip_from) * timescale) / range->timescale;

	for (;;)
	{
		if (start_time + sample_duration <= next_accum_duration)
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
				range->start = 0;
				range->end = 0;
			}
			return VOD_OK;
		}

		sample_duration = parse_be32(cur_entry->duration);
		sample_count = parse_be32(cur_entry->count);
		next_accum_duration = accum_duration + (uint64_t)sample_duration * sample_count;
	}

	// skip to the start time within the current entry
	if (start_time > accum_duration)
	{
		skip_count = vod_div_ceil(start_time - accum_duration, sample_duration);
		sample_count -= skip_count;
		frame_index += skip_count;
		accum_duration += (uint64_t)skip_count * sample_duration;
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
			range->start = 0;
			range->end = 0;
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

			sample_duration = parse_be32(cur_entry->duration);
			sample_count = parse_be32(cur_entry->count);
			next_accum_duration = accum_duration + (uint64_t)sample_duration * sample_count;
		}

		// skip to the key frame within the current entry
		skip_count = key_frame_index - frame_index;
		sample_count -= skip_count;
		frame_index = key_frame_index;
		accum_duration += (uint64_t)skip_count * sample_duration;
	}

	// store the first frame info
	first_frame = frame_index;
	context->first_frame_time_offset = accum_duration;

	// calculate the end time and initial alloc size
	initial_alloc_size = 128;

	if (range->end == ULLONG_MAX)
	{
		end_time = ULLONG_MAX;

		if (entries == 1)
		{
			// optimization - pre-allocate the correct size for constant frame rate
			initial_alloc_size = sample_count;
		}
	}
	else
	{
		end_time = ((range->end + context->clip_from) * timescale) / range->timescale;

		if (entries == 1)
		{
			// optimization - pre-allocate the correct size for constant frame rate
			sample_duration = parse_be32(cur_entry->duration);
			if (sample_duration == 0)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: sample duration is zero (1)");
				return VOD_BAD_DATA;
			}

			initial_alloc_size = (end_time - start_time) / sample_duration + 1;
			if (initial_alloc_size > sample_count)
			{
				initial_alloc_size = sample_count;
			}
		}
	}

	// initialize the frames array
	if (initial_alloc_size > context->parse_params.max_frame_count)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stts_atom: initial alloc size %uD exceeds the max frame count %uD", initial_alloc_size, context->parse_params.max_frame_count);
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
			if (sample_duration != 0 && 
				end_time != ULLONG_MAX && 
				end_time < accum_duration + ((uint64_t)UINT_MAX) * sample_duration)
			{
				cur_count = vod_div_ceil(end_time - accum_duration, sample_duration);
				cur_count = vod_min(cur_count, sample_count);
			}
			else
			{
				cur_count = sample_count;
			}

			if (cur_count > context->parse_params.max_frame_count - frames_array.nelts)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: frame count exceeds the limit %uD", context->parse_params.max_frame_count);
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
			accum_duration += (uint64_t)cur_count * sample_duration;

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

			sample_duration = parse_be32(cur_entry->duration);
			sample_count = parse_be32(cur_entry->count);
		}
	}

	if (context->parse_params.clip_to == UINT_MAX)
	{
		clip_to = ULLONG_MAX;
	}
	else
	{
		clip_to = ((uint64_t)context->parse_params.clip_to * timescale) / range->timescale;
	}

	// parse the frame durations until the next key frame
	if (context->stss_entries != 0)
	{
		if (frames_array.nelts == 0)
		{
			context->first_frame_time_offset -= clip_from_accum_duration;
			range->start = 0;
			range->end = 0;
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

			if (sample_duration != 0 &&
				clip_to != ULLONG_MAX &&
				clip_to < accum_duration + ((uint64_t)UINT_MAX) * sample_duration)
			{
				cur_count = vod_div_ceil(clip_to - accum_duration, sample_duration);
				sample_count = vod_min(cur_count, sample_count);
			}

			if (sample_count > context->parse_params.max_frame_count - frames_array.nelts)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stts_atom: frame count exceeds the limit %uD", context->parse_params.max_frame_count);
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
			accum_duration += (uint64_t)sample_count * sample_duration;
			
			for (cur_frame_limit = cur_frame + sample_count; cur_frame < cur_frame_limit; cur_frame++)
			{
				cur_frame->duration = sample_duration;
				cur_frame->pts_delay = 0;
			}

			if (frame_index >= key_frame_index || accum_duration >= clip_to)
			{
				break;
			}

			// fetch next sample duration and count
			cur_entry++;
			if (cur_entry >= last_entry)
			{
				break;
			}

			sample_duration = parse_be32(cur_entry->duration);
			sample_count = parse_be32(cur_entry->count);
		}

		// adjust the start / end parameters so that next tracks will align according to this one
		range->timescale = timescale;
		range->start = context->first_frame_time_offset - clip_from_accum_duration;
		if (key_frame_index != UINT_MAX)
		{
			range->end = accum_duration - clip_from_accum_duration;
		}
		else
		{
			range->end = ULLONG_MAX;
		}
		if (clip_to - clip_from < range->end)
		{
			range->end = clip_to - clip_from;
		}
		context->clip_from = clip_from_accum_duration;
	}

	context->first_frame = first_frame;
	context->last_frame = first_frame + frames_array.nelts;

	if (context->last_frame < context->first_frame)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stts_atom: last frame %uD smaller than first frame %uD", context->last_frame, context->first_frame);
		return VOD_BAD_DATA;
	}

	context->total_frames_duration = accum_duration - context->first_frame_time_offset;
	context->first_frame_time_offset -= clip_from_accum_duration;	
	context->frames = frames_array.elts;
	context->frame_count = frames_array.nelts;

	if (clip_to != ULLONG_MAX &&
		(cur_entry >= last_entry || (accum_duration - clip_from_accum_duration) > clip_to - clip_from))
	{
		context->clip_to = context->parse_params.clip_to - context->parse_params.clip_from;
	}
	else
	{
		context->clip_to = UINT_MAX;
	}
	
	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_ctts_atom_initial_pts_delay(atom_info_t* atom_info, frames_parse_context_t* context)
{
	const ctts_entry_t* first_entry;
	const ctts_entry_t* last_entry;
	const ctts_entry_t* cur_entry;
	vod_status_t rc;
	uint32_t entries;
	uint32_t dts_shift;
	int32_t sample_duration;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}

	if (context->media_info->media_type != MEDIA_TYPE_VIDEO)
	{
		return VOD_OK;
	}

	rc = mp4_parser_validate_ctts_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// estimate dts_shift
	if (entries > MAX_PTS_DELAY_TEST_SAMPLES)
	{
		entries = MAX_PTS_DELAY_TEST_SAMPLES;
	}

	first_entry = (const ctts_entry_t*)(atom_info->ptr + sizeof(ctts_atom_t));
	last_entry = first_entry + entries;

	dts_shift = 0;
	for (cur_entry = first_entry; cur_entry < last_entry; cur_entry++)
	{
		sample_duration = parse_be32(cur_entry->duration);
		if (sample_duration < 0 && (uint32_t)-sample_duration > dts_shift)
		{
			dts_shift = (uint32_t)-sample_duration;
		}
	}

	context->media_info->u.video.initial_pts_delay = dts_shift + parse_be32(first_entry->duration);
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_ctts_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	const ctts_entry_t* first_entry;
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

	first_entry = (const ctts_entry_t*)(atom_info->ptr + sizeof(ctts_atom_t));
	last_entry = first_entry + entries;
	cur_entry = first_entry;

	// parse the first entry
	if (cur_entry >= last_entry)
	{
		return VOD_OK;
	}

	sample_duration = parse_be32(cur_entry->duration);
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

		sample_duration = parse_be32(cur_entry->duration);
		if (sample_duration < 0 && (uint32_t)-sample_duration > dts_shift)
		{
			dts_shift = (uint32_t)-sample_duration;
		}

		sample_count = parse_be32(cur_entry->count);
	}

	// jump to the first frame
	sample_count -= (context->first_frame - frame_index);

	// parse the frames
	for (;;)
	{
		cur_limit = cur_frame + sample_count;
		if (cur_limit > last_frame)
		{
			cur_limit = last_frame;
		}
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

		sample_duration = parse_be32(cur_entry->duration);
		if (sample_duration < 0 && (uint32_t)-sample_duration > dts_shift)
		{
			dts_shift = (uint32_t)-sample_duration;
		}

		sample_count = parse_be32(cur_entry->count);
	}
	
	context->dts_shift = dts_shift;
	if (context->media_info->media_type == MEDIA_TYPE_VIDEO)
	{
		context->media_info->u.video.initial_pts_delay = dts_shift + parse_be32(first_entry->duration);
	}
	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_stco_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	input_frame_t* cur_frame = context->frames;
	input_frame_t* last_frame = cur_frame + context->frame_count;
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
			for (; cur_frame < last_frame; cur_frame++)
			{
				read_be64(cur_pos, cur_frame->offset);
			}
		}
		else
		{
			for (; cur_frame < last_frame; cur_frame++)
			{
				read_be32(cur_pos, cur_frame->offset);
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
		for (; cur_frame < last_frame; cur_frame++)
		{
			if (cur_frame->key_frame != cur_chunk_index)
			{
				// Note: assuming chunk indexes always grow by 1, the way stsc is encoded ensures that's always true
				cur_chunk_index = cur_frame->key_frame;
				read_be64(cur_pos, cur_file_offset);
			}
			cur_frame->offset = cur_file_offset;
			cur_file_offset += cur_frame->size;
		}
	}
	else
	{
		read_be32(cur_pos, cur_file_offset);
		cur_file_offset += context->first_frame_chunk_offset;
		for (; cur_frame < last_frame; cur_frame++)
		{
			if (cur_frame->key_frame != cur_chunk_index)
			{
				// Note: assuming chunk indexes always grow by 1, the way stsc is encoded ensures that's always true
				cur_chunk_index = cur_frame->key_frame;
				read_be32(cur_pos, cur_file_offset);
			}
			cur_frame->offset = cur_file_offset;
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
	uint64_t cur_entry_samples;
	uint32_t entries;
	uint32_t frame_index = 0;
	uint32_t cur_chunk_zero_based;
	uint32_t cur_chunk;
	uint32_t next_chunk;
	uint32_t samples_per_chunk;
	uint32_t cur_sample;
	uint32_t skip_chunks;
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
	if (next_chunk != 1)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsc_atom: first chunk index is not 1");
		return VOD_BAD_DATA;
	}

	if (frame_index < context->first_frame)
	{
		// skip to the relevant entry
		for (; cur_entry + 1 < last_entry; cur_entry++, frame_index += cur_entry_samples)
		{
			cur_chunk = next_chunk;
			next_chunk = parse_be32(cur_entry[1].first_chunk);
			if (next_chunk <= cur_chunk)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: chunk index %uD is smaller than the previous index %uD (1)", next_chunk, cur_chunk);
				return VOD_BAD_DATA;
			}

			samples_per_chunk = parse_be32(cur_entry->samples_per_chunk);
			if (samples_per_chunk == 0)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: invalid samples per chunk %uD", samples_per_chunk);
				return VOD_BAD_DATA;
			}

			cur_entry_samples = (uint64_t)(next_chunk - cur_chunk) * samples_per_chunk;
			if (cur_entry_samples > UINT_MAX - frame_index)		// integer overflow protection
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsc_atom: number of samples per entry %uL is too big", cur_entry_samples);
				return VOD_BAD_DATA;
			}

			if (frame_index + cur_entry_samples > context->first_frame)
			{
				next_chunk = cur_chunk;		// the first frame is within the current entry, revert the value of next chunk
				break;
			}
		}
	}

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

	// skip samples until we get to the first frame
	skip_chunks = (context->first_frame - frame_index) / samples_per_chunk;
	if (skip_chunks >= next_chunk - cur_chunk)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsc_atom: failed to find first frame chunk");
		return VOD_UNEXPECTED;
	}
	cur_chunk += skip_chunks;

	frame_index += skip_chunks * samples_per_chunk;

	context->first_chunk_frame_index = frame_index;
	cur_sample = samples_per_chunk - (context->first_frame - frame_index);

	for (;;)
	{
		// map the frames to the chunk they reside in
		cur_chunk_zero_based = cur_chunk - 1;

		for (; cur_sample; cur_sample--)
		{
			if (cur_frame >= last_frame)
			{
				return VOD_OK;
			}

			// Note: using the key_frame field to hold the chunk index in order to avoid allocating
			//		extra room for it (it's temporary)
			cur_frame->key_frame = cur_chunk_zero_based;
			cur_frame++;
		}

		// move to the next chunk
		cur_chunk++;
		if (cur_chunk < next_chunk)
		{
			cur_sample = samples_per_chunk;
			continue;
		}

		// move to the next entry
		cur_entry++;
		if (cur_entry >= last_entry)
		{
			break;
		}

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

		cur_sample = samples_per_chunk;
	}

	// unexpected - didn't reach the last frame
	vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
		"mp4_parser_parse_stsc_atom: failed to get the chunk indexes for all frames");
	return VOD_BAD_DATA;
}

static vod_status_t
mp4_parser_parse_stsz_atom_key_frame_bitrate(
	frames_parse_context_t* context, 
	atom_info_t* stsz, 
	atom_info_t* stss)
{
	const uint8_t* stsz_data;
	uint32_t* cur_pos;
	uint32_t* end_pos;
	uint32_t stss_entries;
	uint32_t stsz_entries;
	uint32_t sample_count;
	uint32_t uniform_size;
	uint32_t frame_index;
	uint32_t field_size;
	uint64_t total_size;
	vod_status_t rc;

	// validate stss
	if (stss->size == 0) 			// optional atom
	{
		return VOD_OK;
	}

	rc = mp4_parser_validate_stss_atom(context->request_context, stss, &stss_entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (stss_entries == 0)
	{
		return VOD_OK;
	}

	// validate stsz
	rc = mp4_parser_validate_stsz_atom(
		context->request_context, 
		stsz, 
		0, 
		&uniform_size, 
		&field_size, 
		&stsz_entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (uniform_size != 0)
	{
		context->media_info->u.video.key_frame_bitrate =
			((uint64_t)uniform_size * stss_entries * context->media_info->timescale * 8) /
			(context->media_info->full_duration);
		return VOD_OK;
	}

	// calculate the total size of sample key frames
	sample_count = (stss_entries > MAX_KEY_FRAME_BITRATE_TEST_SAMPLES) ? 
		MAX_KEY_FRAME_BITRATE_TEST_SAMPLES : stss_entries;

	cur_pos = (uint32_t*)(stss->ptr + sizeof(stss_atom_t));
	end_pos = cur_pos + sample_count;
	stsz_data = stsz->ptr + sizeof(stsz_atom_t);
	total_size = 0;

	switch (field_size)
	{
	case 32:
		for (; cur_pos < end_pos; cur_pos++)
		{
			frame_index = parse_be32(cur_pos) - 1;
			if (frame_index >= stsz_entries)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsz_atom_key_frame_bitrate: invalid frame index %uD (1)", frame_index);
				return VOD_BAD_DATA;
			}

			total_size += parse_be32((const uint32_t*)stsz_data + frame_index);
		}
		break;

	case 16:
		for (; cur_pos < end_pos; cur_pos++)
		{
			frame_index = parse_be32(cur_pos) - 1;
			if (frame_index >= stsz_entries)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsz_atom_key_frame_bitrate: invalid frame index %uD (2)", frame_index);
				return VOD_BAD_DATA;
			}

			total_size += parse_be16((const uint16_t*)stsz_data + frame_index);
		}
		break;

	case 8:
		for (; cur_pos < end_pos; cur_pos++)
		{
			frame_index = parse_be32(cur_pos) - 1;
			if (frame_index >= stsz_entries)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsz_atom_key_frame_bitrate: invalid frame index %uD (3)", frame_index);
				return VOD_BAD_DATA;
			}

			total_size += stsz_data[frame_index];
		}
		break;

	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_stsz_atom_key_frame_bitrate: unsupported field size %uD", field_size);
		return VOD_BAD_DATA;
	}

	// estimate the bitrate
	context->media_info->u.video.key_frame_bitrate = 
		(total_size * stss_entries * context->media_info->timescale * 8) / 
		(sample_count * context->media_info->full_duration);
	return VOD_OK;
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
	uint32_t cur_size;
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
		context->total_frames_size += (uint64_t)uniform_size * context->frame_count;
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
			read_be32(cur_pos, cur_size);
			if (cur_size > MAX_FRAME_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mp4_parser_parse_stsz_atom: frame size %uD too big", cur_size);
				return VOD_BAD_DATA;
			}
			context->total_frames_size += cur_size;
			cur_frame->size = cur_size;
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
			read_be16(cur_pos, cur_size);
			// Note: no need to validate the size here, since MAX_UINT16 < MAX_FRAME_SIZE
			context->total_frames_size += cur_size;
			cur_frame->size = cur_size;
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
			cur_size = *cur_pos++;
			// Note: no need to validate the size here, since MAX_UINT8 < MAX_FRAME_SIZE
			context->total_frames_size += cur_size;
			cur_frame->size = cur_size;
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
mp4_parser_parse_sinf_atoms(void* ctx, atom_info_t* atom_info)
{
	metadata_parse_context_t* context = (metadata_parse_context_t*)ctx;

	if (atom_info->name != ATOM_NAME_FRMA)
	{
		return VOD_OK;
	}

	if (atom_info->size < sizeof(uint32_t))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_sinf_atoms: frma atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	context->media_info.format = parse_le32(atom_info->ptr);

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_sinf_atom(atom_info_t* atom_info, metadata_parse_context_t* context)
{
	context->sinf_atom = *atom_info;

	return mp4_parser_parse_atoms(
		context->request_context, 
		atom_info->ptr, 
		atom_info->size, 
		TRUE, 
		mp4_parser_parse_sinf_atoms, 
		context);
}

static vod_status_t 
mp4_parser_parse_video_extra_data_atom(void* ctx, atom_info_t* atom_info)
{
	metadata_parse_context_t* context = (metadata_parse_context_t*)ctx;
	dovi_video_media_info_t* dovi;
	
	switch (atom_info->name)
	{
	case ATOM_NAME_SINF:
		return mp4_parser_parse_sinf_atom(atom_info, context);

	case ATOM_NAME_DVCC:
	case ATOM_NAME_DVVC:
		if (atom_info->size <= 3)
		{
			return VOD_OK;
		}

		dovi = &context->media_info.u.video.dovi;
		dovi->profile = atom_info->ptr[2] >> 1;
		dovi->level = ((atom_info->ptr[2] & 1) << 5) | (atom_info->ptr[3] >> 3);
		return VOD_OK;

	case ATOM_NAME_AVCC:
	case ATOM_NAME_HVCC:
	case ATOM_NAME_VPCC:
	case ATOM_NAME_AV1C:
		break;			// handled outside the switch

	default:
		return VOD_OK;
	}
	
	context->media_info.extra_data.len = atom_info->size;
	context->media_info.extra_data.data = (u_char*)atom_info->ptr;
	
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
	vod_status_t rc;
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
		context->media_info.extra_data.len = len;
		context->media_info.extra_data.data = (u_char*)stream->cur_pos;

		rc = codec_config_mp4a_config_parse(
			context->request_context, 
			&context->media_info.extra_data, 
			&context->media_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return VOD_OK;
}

static uint64_t
mp4_parser_get_ac3_channel_layout(uint8_t acmod, uint8_t lfeon, uint16_t chan_loc)
{
	static uint64_t acmod_channel_layout[] = {
		VOD_CH_LAYOUT_STEREO,
		VOD_CH_LAYOUT_MONO,
		VOD_CH_LAYOUT_STEREO,
		VOD_CH_LAYOUT_SURROUND,
		VOD_CH_LAYOUT_2_1,
		VOD_CH_LAYOUT_4POINT0,
		VOD_CH_LAYOUT_2_2,
		VOD_CH_LAYOUT_5POINT0
	};

	static uint64_t lfeon_channel_layout[] = {
		0,
		VOD_CH_LOW_FREQUENCY
	};

	static uint64_t chan_loc_channel_layout[] = {
		VOD_CH_FRONT_LEFT_OF_CENTER | VOD_CH_FRONT_RIGHT_OF_CENTER,
		VOD_CH_BACK_LEFT | VOD_CH_BACK_RIGHT,
		VOD_CH_BACK_CENTER,
		VOD_CH_TOP_CENTER,
		VOD_CH_SURROUND_DIRECT_LEFT | VOD_CH_SURROUND_DIRECT_RIGHT,
		VOD_CH_WIDE_LEFT | VOD_CH_WIDE_RIGHT,
		VOD_CH_TOP_FRONT_LEFT | VOD_CH_TOP_FRONT_RIGHT,
		VOD_CH_TOP_FRONT_CENTER,
		VOD_CH_LOW_FREQUENCY_2,
	};

	uint64_t result;
	uint32_t i;

	result = acmod_channel_layout[acmod] | lfeon_channel_layout[lfeon];

	for (i = 0; i < vod_array_entries(chan_loc_channel_layout); i++)
	{
		if (chan_loc & (1 << i))
		{
			result |= chan_loc_channel_layout[i];
		}
	}

	return result;
}

static vod_status_t
mp4_parser_parse_dops_atom(metadata_parse_context_t* context, atom_info_t* atom_info)
{
	const dops_atom_t* atom = (const dops_atom_t*)atom_info->ptr;
	u_char* p;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_dops_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	p = vod_alloc(context->request_context->pool, sizeof(opus_extra_data_t));
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_dops_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	context->media_info.extra_data.data = p;

	p = vod_copy(p, OPUS_EXTRA_DATA_MAGIC, sizeof(OPUS_EXTRA_DATA_MAGIC) - 1);
	*p++ = 1;				// version
	*p++ = atom->channels[0];
	write_swap16(p, atom->initial_padding);
	write_swap32(p, atom->sample_rate);
	write_swap16(p, atom->gain);
	*p++ = 0;

	context->media_info.extra_data.len = p - context->media_info.extra_data.data;

	if (context->media_info.extra_data.len != sizeof(opus_extra_data_t))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_dops_atom: invalid result length %uz",
			context->media_info.extra_data.len);
		return VOD_UNEXPECTED;
	}

	context->media_info.codec_delay = (uint64_t)parse_be16(atom->initial_padding) * 1000000000 / 48000;

	return VOD_OK;
}

static vod_status_t 
mp4_parser_parse_audio_atoms(void* ctx, atom_info_t* atom_info)
{
	metadata_parse_context_t* context = (metadata_parse_context_t*)ctx;
	simple_read_stream_t stream;
	vod_status_t rc;
	uint64_t channel_layout;
	uint16_t chan_loc;
	uint8_t acmod;
	uint8_t lfeon;
	int tag;

	switch (atom_info->name)
	{
	case ATOM_NAME_SINF:
		return mp4_parser_parse_sinf_atom(atom_info, context);

	case ATOM_NAME_DOPS:
		return mp4_parser_parse_dops_atom(context, atom_info);

	case ATOM_NAME_ESDS:
		break;			// handled outside the switch

	case ATOM_NAME_DAC3:
		if (atom_info->size <= 1)
		{
			return VOD_OK;
		}

		acmod = (atom_info->ptr[1] >> 3) & 0x7;
		lfeon = (atom_info->ptr[1] >> 2) & 0x1;

		channel_layout = mp4_parser_get_ac3_channel_layout(acmod, lfeon, 0);

		context->media_info.u.audio.channels = vod_get_number_of_set_bits64(channel_layout);
		context->media_info.u.audio.channel_layout = channel_layout;
		return VOD_OK;

	case ATOM_NAME_DEC3:
		if (atom_info->size <= 3)
		{
			return VOD_OK;
		}

		acmod = (atom_info->ptr[3] >> 1) & 0x7;
		lfeon = (atom_info->ptr[3]) & 0x1;

		if (atom_info->size > 5 && (atom_info->ptr[4] >> 1) & 0xf)  // num_dep_sub
		{
			chan_loc = (atom_info->ptr[4] & 0x1) << 8 | atom_info->ptr[5];
		}
		else
		{
			chan_loc = 0;
		}

		channel_layout = mp4_parser_get_ac3_channel_layout(acmod, lfeon, chan_loc);

		context->media_info.u.audio.channels = vod_get_number_of_set_bits64(channel_layout);
		context->media_info.u.audio.channel_layout = channel_layout;

		// set the extra data to the contents of this atom, since it's used
		// as the setup info in hls/sample-aes
		context->media_info.extra_data.len = atom_info->size;
		context->media_info.extra_data.data = (u_char*)atom_info->ptr;
		return VOD_OK;

	default:
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
			mp4_parser_parse_audio_atoms,
			context);
	}
	else
	{
		rc = mp4_parser_parse_audio_atoms(ctx, atom_info);
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
	const u_char* entry_end_pos;
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
		if (size > (uint32_t)(end_pos - cur_pos))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stsd_atom: stsd entry overflow the stream");
			return VOD_BAD_DATA;
		}

		entry_end_pos = cur_pos + size;

		cur_pos += sizeof(stsd_entry_header_t);
		if (size >= 16)
		{
			cur_pos += sizeof(stsd_large_entry_header_t);
		}
		
		cur_pos = skip_function(cur_pos, entry_end_pos, context);
		if (cur_pos == NULL)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stsd_atom: failed to skip audio/video data");
			return VOD_BAD_DATA;
		}

		if (cur_pos > entry_end_pos)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_stsd_atom: stream overflow before reaching extra data");
			return VOD_BAD_DATA;
		}

		if (cur_pos + 8 < entry_end_pos)
		{
			vod_log_buffer(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0, "mp4_parser_parse_stsd_atom: extra data ", cur_pos, entry_end_pos - cur_pos);

			rc = mp4_parser_parse_atoms(context->request_context, cur_pos, entry_end_pos - cur_pos, TRUE, parse_function, context);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		cur_pos = entry_end_pos;
	}
	
	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_saiz_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	const saiz_atom_t* atom;
	const saiz_with_type_atom_t* atom_with_type;
	const uint8_t* first_entry;
	const uint8_t* last_entry;
	const uint8_t* cur_entry;
	uint32_t offset;
	uint8_t default_size;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_saiz_atom: atom size %uL too small (1)", atom_info->size);
		return VOD_BAD_DATA;
	}

	atom = (const saiz_atom_t*)atom_info->ptr;

	if ((atom->flags[2] & 0x01) != 0)
	{
		if (atom_info->size < sizeof(*atom_with_type))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_saiz_atom: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}

		atom_with_type = (const saiz_with_type_atom_t*)atom_info->ptr;

		default_size = atom_with_type->default_size[0];
		first_entry = (const uint8_t*)(atom_with_type + 1);
	}
	else
	{
		default_size = atom->default_size[0];
		first_entry = (const uint8_t*)(atom + 1);
	}

	context->encryption_info.default_auxiliary_sample_size = default_size;

	if (default_size != 0)
	{
		context->auxiliary_info_start_offset = context->first_frame * default_size;
		context->auxiliary_info_end_offset = context->last_frame * default_size;
		return VOD_OK;
	}

	if (first_entry + context->last_frame > atom_info->ptr + atom_info->size)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_saiz_atom: atom too small to hold %uD entries", context->last_frame);
		return VOD_BAD_DATA;
	}

	// save the sample sizes (used for passthrough encryption)
	context->encryption_info.auxiliary_sample_sizes = vod_alloc(context->request_context->pool, context->frame_count);
	if (context->encryption_info.auxiliary_sample_sizes == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_saiz_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(context->encryption_info.auxiliary_sample_sizes, first_entry + context->first_frame, context->frame_count);

	// get the start offset
	offset = 0;
	last_entry = first_entry + context->first_frame;
	for (cur_entry = first_entry; cur_entry < last_entry; cur_entry++)
	{
		offset += *cur_entry;
	}

	context->auxiliary_info_start_offset = offset;

	// get the end offset
	last_entry = first_entry + context->last_frame;
	for (; cur_entry < last_entry; cur_entry++)
	{
		offset += *cur_entry;
	}

	context->auxiliary_info_end_offset = offset;

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_senc_atom(atom_info_t* atom_info, frames_parse_context_t* context)
{
	const senc_atom_t* atom;
	size_t auxiliary_info_size;

	if (atom_info->size == 0 || context->auxiliary_info_start_offset >= context->auxiliary_info_end_offset)		// optional atom
	{
		return VOD_OK;
	}

	if (atom_info->size < sizeof(*atom) + context->auxiliary_info_end_offset)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_senc_atom: atom smaller than end offset %uD", context->auxiliary_info_end_offset);
		return VOD_BAD_DATA;
	}

	atom = (const senc_atom_t*)atom_info->ptr;

	auxiliary_info_size = context->auxiliary_info_end_offset - context->auxiliary_info_start_offset;
	context->encryption_info.auxiliary_info = vod_alloc(context->request_context->pool, auxiliary_info_size);
	if (context->encryption_info.auxiliary_info == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_parse_senc_atom: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memcpy(
		context->encryption_info.auxiliary_info, 
		atom_info->ptr + sizeof(*atom) + context->auxiliary_info_start_offset, 
		auxiliary_info_size);
	context->encryption_info.auxiliary_info_end = context->encryption_info.auxiliary_info + auxiliary_info_size;
	context->encryption_info.use_subsamples = (atom->flags[2] & 0x02);

	return VOD_OK;
}

static vod_status_t
mp4_parser_parse_mvhd_atom(process_moov_context_t* context, atom_info_t* atom_info)
{
	const mvhd_atom_t* atom = (const mvhd_atom_t*)atom_info->ptr;
	const mvhd64_atom_t* atom64;
	mp4_base_metadata_t* result = context->result;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_parse_mvhd_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_parser_parse_mvhd_atom: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}

		atom64 = (const mvhd64_atom_t*)atom_info->ptr;
		result->mvhd_timescale = parse_be32(atom64->timescale);
	}
	else
	{
		result->mvhd_timescale = parse_be32(atom->timescale);
	}

	return VOD_OK;
}

static vod_status_t
mp4_parser_process_moov_atom_callback(void* ctx, atom_info_t* atom_info)
{
	process_moov_context_t* context = (process_moov_context_t*)ctx;
	save_relevant_atoms_context_t save_atoms_context;
	metadata_parse_context_t metadata_parse_context;
	mp4_track_base_metadata_t* result_track;
	mp4_base_metadata_t* result = context->result;
	trak_atom_infos_t trak_atom_infos;
	media_sequence_t* sequence;
	uint32_t duration_millis;
	uint32_t track_index;
	uint32_t bitrate;
	bool_t extra_data_required;
	vod_status_t rc;
	int parse_type;

	switch (atom_info->name)
	{
	case ATOM_NAME_MVHD:
		return mp4_parser_parse_mvhd_atom(context, atom_info);

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

	// inherit the sequence tags
	sequence = context->parse_params.source->sequence;
	metadata_parse_context.media_info.tags = sequence->tags;

	rc = mp4_parser_parse_hdlr_atom(&trak_atom_infos.hdlr, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// udta stream name
	if ((context->parse_params.parse_type & PARSE_FLAG_UDTA_NAME) != 0)
	{
		rc = mp4_parser_parse_udta_name_atom(&trak_atom_infos.udta_name, &metadata_parse_context);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// get the codec type and extra data
	rc = mp4_parser_parse_stsd_atom(&trak_atom_infos.stsd, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// make sure the codec is supported
	extra_data_required = TRUE;
	metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_INVALID;
	switch (metadata_parse_context.media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		switch (metadata_parse_context.media_info.format)
		{
		case FORMAT_AVC1:
		case FORMAT_h264:
		case FORMAT_H264:
		case FORMAT_DVA1:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_AVC;
			break;

		case FORMAT_HEV1:
		case FORMAT_HVC1:
		case FORMAT_DVH1:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_HEVC;
			break;

		case FORMAT_VP09:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_VP9;
			break;

		case FORMAT_AV1:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_AV1;
			break;
		}
		break;

	case MEDIA_TYPE_AUDIO:
		switch (metadata_parse_context.media_info.format)
		{
		case FORMAT_MP4A:
			switch (metadata_parse_context.media_info.u.audio.object_type_id)
			{
			case 0x40:
			case 0x66:
			case 0x67:
			case 0x68:
				metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_AAC;
				break;

			case 0x69:
			case 0x6B:
				metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_MP3;
				extra_data_required = FALSE;
				break;

			case 0xa9:
				metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_DTS;
				extra_data_required = FALSE;
				break;
			}
			break;

		case FORMAT_AC3:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_AC3;
			extra_data_required = FALSE;
			break;

		case FORMAT_EAC3:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_EAC3;
			extra_data_required = FALSE;
			break;

		case FORMAT_FLAC:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_FLAC;
			extra_data_required = FALSE;
			break;

		case FORMAT_OPUS:
			metadata_parse_context.media_info.codec_id = VOD_CODEC_ID_OPUS;
			break;
		}
		break;
	}

	if (metadata_parse_context.media_info.codec_id == VOD_CODEC_ID_INVALID)
	{
		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: unsupported format - media type %uD format 0x%uxD object type id 0x%uxD",
			metadata_parse_context.media_info.media_type, metadata_parse_context.media_info.format, (uint32_t)metadata_parse_context.media_info.u.audio.object_type_id);
		return VOD_OK;
	}

	if (!vod_codec_in_mask(metadata_parse_context.media_info.codec_id, metadata_parse_context.parse_params.codecs_mask))
	{
		vod_log_debug2(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: codec %uD not supported for this request mask 0x%xd",
			metadata_parse_context.media_info.codec_id, metadata_parse_context.parse_params.codecs_mask);
		return VOD_OK;
	}

	// make sure we got the extra data
	if (extra_data_required && metadata_parse_context.media_info.extra_data.data == NULL)
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

	// get the duration and language
	rc = mp4_parser_parse_mdhd_atom(&trak_atom_infos.mdhd, &metadata_parse_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// inherit the sequence language and label
	if (sequence->tags.label.len != 0)
	{
		metadata_parse_context.media_info.tags.label = sequence->tags.label;

		// Note: it is not possible for the sequence to have a language without a label,
		//              since a default label will be assigned according to the language
		if (sequence->tags.lang_str.len != 0)
		{
			metadata_parse_context.media_info.tags.lang_str = sequence->tags.lang_str;
			metadata_parse_context.media_info.tags.language = sequence->tags.language;
		}
	}

	// check whether we should include this track
	track_index = context->track_indexes[metadata_parse_context.media_info.media_type]++;
	if (!vod_is_bit_set(context->parse_params.required_tracks_mask[metadata_parse_context.media_info.media_type], track_index))
	{
		return VOD_OK;
	}

	// filter by language
	if (context->parse_params.langs_mask != NULL &&
		metadata_parse_context.media_info.media_type == MEDIA_TYPE_AUDIO &&
		!vod_is_bit_set(context->parse_params.langs_mask, metadata_parse_context.media_info.tags.language))
	{
		return VOD_OK;
	}

	// parse the edit list
	parse_type = context->parse_params.parse_type;

	if ((parse_type & PARSE_FLAG_EDIT_LIST) != 0)
	{
		rc = mp4_parser_parse_elst_atom(&trak_atom_infos.elst, &metadata_parse_context);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// Microsoft H.264 Encoder generates a wrong duration in mdhd, pull it from stts
	if (context->ftyp_atom.len >= sizeof(uint32_t) && 
		parse_le32(context->ftyp_atom.data) == FTYP_TYPE_MP42)
	{
		rc = mp4_parser_parse_stts_atom_total_duration_only(&trak_atom_infos.stts, &metadata_parse_context);
		if (rc != VOD_OK)
		{
			return rc;
		}
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
	else
	{
		metadata_parse_context.media_info.duration = metadata_parse_context.media_info.full_duration;
	}

	rc = media_format_finalize_track(
		context->request_context,
		parse_type,
		&metadata_parse_context.media_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// add to the result array
	if (result->base.tracks.nelts > MAX_TRACK_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: track count exceeded the limit of %i", (ngx_int_t)MAX_TRACK_COUNT);
		return VOD_BAD_REQUEST;
	}

	result_track = vod_array_push(&result->base.tracks);
	if (result_track == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_parser_process_moov_atom_callback: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}

	// inherit bitrate from sequence
	bitrate = sequence->bitrate[metadata_parse_context.media_info.media_type];
	if (bitrate != 0)
	{
		metadata_parse_context.media_info.bitrate = bitrate;
	}

	bitrate = sequence->avg_bitrate[metadata_parse_context.media_info.media_type];
	metadata_parse_context.media_info.avg_bitrate = bitrate;

	result_track->trak_atom_infos = trak_atom_infos;
	result_track->media_info = metadata_parse_context.media_info;
	result_track->sinf_atom = metadata_parse_context.sinf_atom;
	result_track->track_index = track_index;

	// update max duration / track index
	if (result->base.duration == 0 ||
		result->base.duration * metadata_parse_context.media_info.timescale < metadata_parse_context.media_info.duration * result->base.timescale)
	{
		result->base.duration = metadata_parse_context.media_info.duration;
		result->base.timescale = metadata_parse_context.media_info.timescale;
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
	{ mp4_parser_parse_ctts_atom_initial_pts_delay,			offsetof(trak_atom_infos_t, ctts), PARSE_FLAG_INITIAL_PTS_DELAY },
	{ mp4_parser_parse_stsc_atom,							offsetof(trak_atom_infos_t, stsc), PARSE_FLAG_FRAMES_OFFSET },
	{ mp4_parser_parse_stsz_atom,							offsetof(trak_atom_infos_t, stsz), PARSE_FLAG_FRAMES_SIZE },
	{ mp4_parser_parse_stsz_atom_total_size_estimate_only,	offsetof(trak_atom_infos_t, stsz), PARSE_FLAG_TOTAL_SIZE_ESTIMATE },
	{ mp4_parser_parse_stco_atom,							offsetof(trak_atom_infos_t, stco), PARSE_FLAG_FRAMES_OFFSET},
	{ mp4_parser_parse_stss_atom,							offsetof(trak_atom_infos_t, stss), PARSE_FLAG_FRAMES_IS_KEY },
	{ mp4_parser_parse_saiz_atom,							offsetof(trak_atom_infos_t, saiz), PARSE_FLAG_FRAMES_OFFSET },
	{ mp4_parser_parse_senc_atom,							offsetof(trak_atom_infos_t, senc), PARSE_FLAG_FRAMES_OFFSET },
	{ NULL, 0, 0 }
};

vod_status_t 
mp4_parser_parse_basic_metadata(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	process_moov_context_t context;
	mp4_base_metadata_t* metadata;
	vod_status_t rc;

	metadata = vod_alloc(request_context->pool, sizeof(*metadata));
	if (metadata == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_parse_basic_metadata: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(metadata, sizeof(*metadata));
	if (vod_array_init(&metadata->base.tracks, request_context->pool, 2, sizeof(mp4_track_base_metadata_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_parse_basic_metadata: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	context.request_context = request_context;
	context.parse_params = *parse_params;
	vod_memzero(context.track_indexes, sizeof(context.track_indexes));
	context.ftyp_atom = metadata_parts[MP4_METADATA_PART_FTYP];
	context.result = metadata;

	rc = mp4_parser_parse_atoms(
		request_context, 
		metadata_parts[MP4_METADATA_PART_MOOV].data,
		metadata_parts[MP4_METADATA_PART_MOOV].len,
		TRUE, 
		&mp4_parser_process_moov_atom_callback, 
		&context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (metadata->mvhd_timescale == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_parse_basic_metadata: mvhd timescale was not set");
		return VOD_BAD_DATA;
	}

	*result = &metadata->base;

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

static void 
mp4_parser_strip_stsd_encryption(
	raw_atom_t* dest_stsd_atom,
	mp4_track_base_metadata_t* track)
{
	atom_info_t* src_sinf_atom = &track->sinf_atom;
	atom_info_t* src_stsd_atom = &track->trak_atom_infos.stsd;
	uint32_t original_format = track->media_info.format;
	uint32_t stsd_entry_size;
	size_t sinf_size = src_sinf_atom->header_size + src_sinf_atom->size;
	size_t sinf_offset;
	u_char* dest = (u_char*)dest_stsd_atom->ptr;
	u_char* p;

	// strip off the sinf atom
	sinf_offset = (src_sinf_atom->ptr - src_sinf_atom->header_size) - (src_stsd_atom->ptr - src_stsd_atom->header_size);
	dest_stsd_atom->size -= sinf_size;
	vod_memmove(dest + sinf_offset, dest + sinf_offset + sinf_size, dest_stsd_atom->size - sinf_offset);

	// update the stsd entry size + format
	p = dest + dest_stsd_atom->header_size + sizeof(stsd_atom_t);
	stsd_entry_size = parse_be32(p) - sinf_size;
	write_be32(p, stsd_entry_size);
	write_le32(p, original_format);

	// update the stsd atom size
	// Note: assuming 32 bit size, in case of 64 bit the atom will get corrupted, but this is very unlikely
	p = dest;
	write_be32(p, dest_stsd_atom->size);
}

static int
mp4_parser_compare_tracks(const void *s1, const void *s2)
{
	mp4_track_base_metadata_t* track1 = (mp4_track_base_metadata_t*)s1;
	mp4_track_base_metadata_t* track2 = (mp4_track_base_metadata_t*)s2;

	if (track1->media_info.media_type != track2->media_info.media_type)
	{
		return track1->media_info.media_type - track2->media_info.media_type;
	}

	return track1->track_index - track2->track_index;
}

vod_status_t
mp4_parser_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base_metadata,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	mp4_base_metadata_t* metadata = vod_container_of(base_metadata, mp4_base_metadata_t, base);
	mp4_track_base_metadata_t* first_track = (mp4_track_base_metadata_t*)metadata->base.tracks.elts;
	mp4_track_base_metadata_t* last_track = first_track + metadata->base.tracks.nelts;
	mp4_track_base_metadata_t* cur_track;
	frames_parse_context_t context;
	media_clip_source_t* source;
	frames_source_t* frames_source;
	media_track_t* result_track;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	const trak_atom_parser_t* cur_parser;
	void* frames_source_context;
	vod_status_t rc;
	vod_array_t tracks;
	uint64_t last_offset;
	uint32_t media_type;

	if (vod_array_init(&tracks, request_context->pool, 2, sizeof(media_track_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_parse_frames: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(result, sizeof(*result));

	// in case we need to parse the frame sizes, we already find the total size
	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_SIZE) != 0)
	{
		parse_params->parse_type &= ~PARSE_FLAG_TOTAL_SIZE_ESTIMATE;
	}

	if (segmenter->align_to_key_frames)
	{
		// sort the streams - video first
		qsort(first_track, metadata->base.tracks.nelts, sizeof(*first_track), mp4_parser_compare_tracks);
	}

	// initialize the context
	context.request_context = request_context;
	context.parse_params = *parse_params;
	context.clip_from = rescale_time(parse_params->clip_from, 1000, parse_params->range->timescale);
	context.mvhd_timescale = metadata->mvhd_timescale;

	source = parse_params->source;

	for (cur_track = first_track; cur_track < last_track; cur_track++)
	{
		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_parse_frames: parsing range %uL-%uL scale %uD",
			context.parse_params.range->start,
			context.parse_params.range->end,
			context.parse_params.range->timescale);

		media_type = cur_track->media_info.media_type;

		// update the media info on the context
		context.media_info = &cur_track->media_info;

		// reset the output part of the context
		vod_memzero((u_char*)&context + offsetof(frames_parse_context_t, stss_start_pos),
			sizeof(context) - offsetof(frames_parse_context_t, stss_start_pos));

		if (cur_track == first_track &&
			media_type == MEDIA_TYPE_VIDEO &&
			cur_track->trak_atom_infos.stss.size != 0 &&
			segmenter->align_to_key_frames)
		{
			rc = mp4_parser_validate_stss_atom(context.request_context, &cur_track->trak_atom_infos.stss, &context.stss_entries);
			if (rc != VOD_OK)
			{
				return rc;
			}

			context.stss_start_pos = (const uint32_t*)(cur_track->trak_atom_infos.stss.ptr + sizeof(stss_atom_t));
		}

		for (cur_parser = trak_atom_parsers; cur_parser->parse; cur_parser++)
		{
			if ((parse_params->parse_type & cur_parser->flag) == 0)
			{
				continue;
			}

			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "mp4_parser_parse_frames: running parser 0x%xD", cur_parser->flag);
			rc = cur_parser->parse((atom_info_t*)((u_char*)&cur_track->trak_atom_infos + cur_parser->offset), &context);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		// estimate the bitrate from frame size if no bitrate was read from the file
		if (cur_track->media_info.full_duration > 0)
		{
			if (cur_track->media_info.bitrate == 0)
			{
				cur_track->media_info.bitrate = (uint32_t)((context.total_frames_size * cur_track->media_info.timescale * 8) / cur_track->media_info.full_duration);
			}

			if ((parse_params->parse_type & PARSE_FLAG_KEY_FRAME_BITRATE) != 0 &&
				media_type == MEDIA_TYPE_VIDEO)
			{
				rc = mp4_parser_parse_stsz_atom_key_frame_bitrate(
					&context,
					&cur_track->trak_atom_infos.stsz,
					&cur_track->trak_atom_infos.stss);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
		}

		result_track = vod_array_push(&tracks);
		if (result_track == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_parser_parse_frames: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}

		// init the frames source
		frames_source = &frames_source_cache;
		rc = frames_source_cache_init(
			request_context,
			read_cache_state,
			source,
			media_type,
			&frames_source_context);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (context.encryption_info.auxiliary_info < context.encryption_info.auxiliary_info_end)
		{
#if (VOD_HAVE_OPENSSL_EVP)
			if (source->encryption.key.len == 0)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_parse_frames: media is encrypted and no decryption key was supplied");
				return VOD_BAD_REQUEST;
			}

			if (source->encryption.key.len != MP4_AES_CTR_KEY_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_parse_frames: invalid encryption key len %uz", source->encryption.key.len);
				return VOD_BAD_MAPPING;
			}

			if (source->encryption.scheme != MCS_ENC_CENC)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_parse_frames: invalid encryption scheme type %d", source->encryption.scheme);
				return VOD_BAD_MAPPING;
			}

			rc = mp4_cenc_decrypt_init(
				request_context,
				frames_source,
				frames_source_context,
				source->encryption.key.data,
				&context.encryption_info,
				&frames_source_context);
			if (rc != VOD_OK)
			{
				return rc;
			}

			frames_source = &mp4_cenc_decrypt_frames_source;
#else
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"mp4_parser_parse_frames: decryption is not supported, recompile with openssl to enable it");
			return VOD_BAD_REQUEST;
#endif // VOD_HAVE_OPENSSL_EVP
		}

		result_track->frames.next = NULL;
		result_track->frames.frames_source = frames_source;
		result_track->frames.frames_source_context = frames_source_context;
		result_track->frames.first_frame = context.frames;
		result_track->frames.last_frame = context.frames + context.frame_count;
		result_track->frames.clip_to = context.clip_to;

		// copy the result
		result_track->media_info = cur_track->media_info;
		result_track->encryption_info = context.encryption_info;
		result_track->index = cur_track->track_index;
		result_track->frame_count = context.frame_count;
		result_track->key_frame_count = context.key_frame_count;
		result_track->total_frames_size = context.total_frames_size;
		result_track->total_frames_duration = context.total_frames_duration;
		result_track->first_frame_index = context.first_frame;
		result_track->first_frame_time_offset = context.first_frame_time_offset;
		result_track->clip_from_frame_offset = context.clip_from_frame_offset;
		result_track->source_clip = NULL;

		// update the last offset of the source clip
		if (context.frame_count > 0 && 
			vod_all_flags_set(parse_params->parse_type, PARSE_FLAG_FRAMES_SIZE | PARSE_FLAG_FRAMES_OFFSET))
		{
			last_frame = result_track->frames.last_frame - 1;
			last_offset = last_frame->offset + last_frame->size;
			if (last_offset > source->last_offset)
			{
				source->last_offset = last_offset;
			}
		}

		// copy raw atoms
		if ((parse_params->parse_type & PARSE_FLAG_SAVE_RAW_ATOMS) != 0)
		{
			rc = mp4_parser_copy_raw_atoms(request_context, result_track->raw_atoms, &cur_track->trak_atom_infos);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (cur_track->sinf_atom.ptr != NULL)
			{
				mp4_parser_strip_stsd_encryption(&result_track->raw_atoms[RTA_STSD], cur_track);
			}
		}

		// add the dts_shift to the pts_delay
		cur_frame = context.frames;
		last_frame = cur_frame + context.frame_count;
		for (; cur_frame < last_frame; cur_frame++)
		{
			cur_frame->pts_delay += context.dts_shift;
		}

		result->track_count[media_type]++;
	}

	result->first_track = (media_track_t*)tracks.elts;
	result->last_track = (media_track_t*)tracks.elts + tracks.nelts;
	result->total_track_count = tracks.nelts;

	return VOD_OK;
}

vod_status_t 
mp4_parser_uncompress_moov(
	request_context_t* request_context,
	const u_char* buffer,
	size_t size,
	size_t max_moov_size,
	u_char** out_buffer,
	off_t* moov_offset,
	size_t* moov_size)
{
	save_relevant_atoms_context_t save_atoms_context;
	moov_atom_infos_t moov_atom_infos;
	vod_status_t rc;
#if (VOD_HAVE_ZLIB)
	atom_info_t find_context;
	dcom_atom_t* dcom;
	cmvd_atom_t* cmvd;
	u_char* uncomp_buffer;
	uLongf uncomp_size;
	size_t alloc_size;
	int zrc;
#endif // VOD_HAVE_ZLIB

	// get the relevant atoms
	vod_memzero(&moov_atom_infos, sizeof(moov_atom_infos));
	save_atoms_context.relevant_atoms = relevant_atoms_moov;
	save_atoms_context.result = &moov_atom_infos;
	save_atoms_context.request_context = request_context;
	rc = mp4_parser_parse_atoms(request_context, buffer, size, TRUE, &mp4_parser_save_relevant_atoms_callback, &save_atoms_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (moov_atom_infos.dcom.ptr == NULL || moov_atom_infos.cmvd.ptr == NULL)
	{
		*out_buffer = NULL;
		return VOD_OK;		// non compressed or corrupt, if corrupt, will fail in trak parsing
	}

#if (VOD_HAVE_ZLIB)
	// validate the compression type
	if (moov_atom_infos.dcom.size < sizeof(*dcom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_uncompress_moov: dcom atom size %uL too small", moov_atom_infos.dcom.size);
		return VOD_BAD_DATA;
	}

	dcom = (dcom_atom_t*)moov_atom_infos.dcom.ptr;
	if (parse_le32(dcom->type) != DCOM_TYPE_ZLIB)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_uncompress_moov: dcom type %*s is not zlib", (size_t)sizeof(dcom->type), dcom->type);
		return VOD_BAD_DATA;
	}

	// get the uncompressed size and compressed buffer
	if (moov_atom_infos.cmvd.size < sizeof(*cmvd))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_uncompress_moov: cmvd atom size %uL too small", moov_atom_infos.cmvd.size);
		return VOD_BAD_DATA;
	}

	cmvd = (cmvd_atom_t*)moov_atom_infos.cmvd.ptr;
	alloc_size = parse_be32(cmvd->uncomp_size);

	if (alloc_size > max_moov_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_uncompress_moov: moov size %uz exceeds the max %uz", alloc_size, max_moov_size);
		return VOD_BAD_DATA;
	}

	// uncompress to a new buffer
	uncomp_buffer = vod_alloc(request_context->pool, alloc_size);
	if (uncomp_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_parser_uncompress_moov: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	uncomp_size = alloc_size;
	zrc = uncompress(
		uncomp_buffer,
		&uncomp_size,
		moov_atom_infos.cmvd.ptr + sizeof(*cmvd),
		moov_atom_infos.cmvd.size - sizeof(*cmvd));
	if (zrc != Z_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_uncompress_moov: uncompress failed %d", zrc);
		return VOD_BAD_DATA;
	}

	// find the moov atom
	find_context.ptr = NULL;
	find_context.size = 0;
	find_context.name = ATOM_NAME_MOOV;
	find_context.header_size = 0;

	mp4_parser_parse_atoms(request_context, uncomp_buffer, uncomp_size, TRUE, &mp4_parser_find_atom_callback, &find_context);
	if (find_context.ptr == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_uncompress_moov: failed to find moov atom");
		return VOD_BAD_DATA;
	}

	// return the result
	*out_buffer = uncomp_buffer;
	*moov_offset = find_context.ptr - uncomp_buffer;
	*moov_size = find_context.size;

	return VOD_OK;
#else
	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"mp4_parser_uncompress_moov: compressed moov atom not supported, recompile with zlib to enable it");
	return VOD_BAD_REQUEST;
#endif // VOD_HAVE_ZLIB
}
