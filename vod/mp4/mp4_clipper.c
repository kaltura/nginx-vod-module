#include "mp4_clipper.h"
#include "mp4_parser_base.h"
#include "mp4_format.h"
#include "mp4_write_stream.h"
#include "mp4_defs.h"
#include "../read_stream.h"

// macros
#define set_be32(p, dw)				\
	{								\
	((u_char*)p)[0] = ((dw) >> 24) & 0xFF;	\
	((u_char*)p)[1] = ((dw) >> 16) & 0xFF;	\
	((u_char*)p)[2] = ((dw) >> 8) & 0xFF;	\
	((u_char*)p)[3] = (dw)& 0xFF;			\
	}

#define set_be64(p, qw)				\
	{								\
	((u_char*)p)[0] = ((qw) >> 56) & 0xFF;	\
	((u_char*)p)[1] = ((qw) >> 48) & 0xFF;	\
	((u_char*)p)[2] = ((qw) >> 40) & 0xFF;	\
	((u_char*)p)[3] = ((qw) >> 32) & 0xFF;	\
	((u_char*)p)[4] = ((qw) >> 24) & 0xFF;	\
	((u_char*)p)[5] = ((qw) >> 16) & 0xFF;	\
	((u_char*)p)[6] = ((qw) >> 8) & 0xFF;	\
	((u_char*)p)[7] = (qw) & 0xFF;			\
	}

#define full_atom_start(atom) ((atom).ptr - (atom).header_size)
#define full_atom_size(atom) ((atom).size + (atom).header_size)
#define copy_full_atom(p, atom) p = vod_copy(p, full_atom_start(atom), full_atom_size(atom))
#define write_full_atom(write_context, index, atom)		\
	if (full_atom_size(atom) > 0)						\
	{													\
		mp4_clipper_write_tail(							\
			write_context,								\
			index,										\
			(u_char*)full_atom_start(atom),				\
			full_atom_size(atom));						\
	}

// enums
enum {
	TRAK_ATOM_TKHD,
	TRAK_ATOM_MDHD,
	TRAK_ATOM_HDLR,
	TRAK_ATOM_VMHD,
	TRAK_ATOM_SMHD,
	TRAK_ATOM_DINF,
	TRAK_ATOM_STSD,
	TRAK_ATOM_STTS,
	TRAK_ATOM_STSS,
	TRAK_ATOM_CTTS,
	TRAK_ATOM_STSC,
	TRAK_ATOM_STSZ,
	TRAK_ATOM_STCO,

	TRAK_ATOM_COUNT,
};

enum {
	MP4_CLIPPER_INDEX_FTYP_HEADER,
	MP4_CLIPPER_INDEX_FTYP_DATA,
	MP4_CLIPPER_INDEX_MOOV_HEADER,
	MP4_CLIPPER_INDEX_MVHD_ATOM,
	MP4_CLIPPER_INDEX_MDAT_HEADER,

	MP4_CLIPPER_INDEX_COUNT
};

enum {
	MP4_CLIPPER_TRAK_INDEX_HEADER,
	MP4_CLIPPER_TRAK_INDEX_TKHD_ATOM,
	MP4_CLIPPER_TRAK_INDEX_MDIA_HEADER,
	MP4_CLIPPER_TRAK_INDEX_MDHD_ATOM,
	MP4_CLIPPER_TRAK_INDEX_HDLR_ATOM,
	MP4_CLIPPER_TRAK_INDEX_MINF_HEADER,
	MP4_CLIPPER_TRAK_INDEX_VMHD_ATOM,
	MP4_CLIPPER_TRAK_INDEX_SMHD_ATOM,
	MP4_CLIPPER_TRAK_INDEX_DINF_ATOM,
	MP4_CLIPPER_TRAK_INDEX_STBL_HEADER,
	MP4_CLIPPER_TRAK_INDEX_STSD_ATOM,
	MP4_CLIPPER_TRAK_INDEX_STTS_HEADER,
	MP4_CLIPPER_TRAK_INDEX_STTS_DATA,
	MP4_CLIPPER_TRAK_INDEX_STSS_HEADER,
	MP4_CLIPPER_TRAK_INDEX_STSS_DATA,
	MP4_CLIPPER_TRAK_INDEX_CTTS_HEADER,
	MP4_CLIPPER_TRAK_INDEX_CTTS_DATA,
	MP4_CLIPPER_TRAK_INDEX_STSC_START,
	MP4_CLIPPER_TRAK_INDEX_STSC_DATA,
	MP4_CLIPPER_TRAK_INDEX_STSC_END,
	MP4_CLIPPER_TRAK_INDEX_STSZ_HEADER,
	MP4_CLIPPER_TRAK_INDEX_STSZ_DATA,
	MP4_CLIPPER_TRAK_INDEX_STCO_HEADER,
	MP4_CLIPPER_TRAK_INDEX_STCO_DATA,

	MP4_CLIPPER_TRAK_INDEX_COUNT
};

// typedefs
typedef struct {
	vod_chain_t cl;
	vod_buf_t b;
} vod_chain_buf_t;

typedef struct {
	vod_chain_t** last;
	vod_chain_buf_t* elts;
} mp4_clipper_write_context;

typedef struct {
	uint64_t duration;
	u_char version;
} tkhd_clip_result_t;

typedef struct {
	uint64_t duration;
	u_char version;
} mdhd_clip_result_t;

typedef struct {
	stts_entry_t* first_entry;
	stts_entry_t* last_entry;
	uint32_t first_count;
	uint32_t last_count;
	size_t data_size;
	size_t atom_size;
	uint32_t entries;
} stts_clip_result_t;

typedef struct {
	uint32_t* first_entry;
	uint32_t* last_entry;
	size_t data_size;
	size_t atom_size;
	uint32_t entries;
	uint32_t first_frame;
} stss_clip_result_t;

typedef struct {
	ctts_entry_t* first_entry;
	ctts_entry_t* last_entry;
	uint32_t first_count;
	uint32_t last_count;
	size_t data_size;
	size_t atom_size;
	uint32_t entries;
} ctts_clip_result_t;

typedef struct {
	stsc_entry_t* first_entry;
	uint32_t first_sample_count;
	uint32_t first_entry_samples_per_chunk;
	uint32_t first_entry_sample_desc;
	uint32_t first_chunk;
	bool_t pre_entry;

	stsc_entry_t* last_entry;
	uint32_t last_sample_count;
	uint32_t last_entry_sample_desc;
	uint32_t last_chunk;
	bool_t post_entry;

	size_t atom_size;
	uint32_t entries;
} stsc_clip_result_t;

typedef struct {
	u_char* first_entry;
	uint32_t uniform_size;
	uint32_t field_size;
	size_t data_size;
	size_t atom_size;
	uint32_t entries;
} stsz_clip_result_t;

typedef struct {
	uint32_t entry_size;
	u_char* first_entry;
	u_char* last_entry;
	uint64_t first_frame_chunk_offset;
	size_t data_size;
	size_t atom_size;
	uint32_t entries;
} stco_clip_result_t;

typedef struct {
	atom_info_t atoms[TRAK_ATOM_COUNT];
	tkhd_clip_result_t tkhd;
	mdhd_clip_result_t mdhd;
	stts_clip_result_t stts;
	stss_clip_result_t stss;
	ctts_clip_result_t ctts;
	stsc_clip_result_t stsc;
	stsz_clip_result_t stsz;
	stco_clip_result_t stco;
	size_t stbl_atom_size;
	size_t minf_atom_size;
	size_t mdia_atom_size;
	size_t trak_atom_size;
} parsed_trak_t;

typedef struct {
	request_context_t* request_context;
	media_parse_params_t parse_params;
	uint32_t track_indexes[MEDIA_TYPE_COUNT];
	uint32_t mvhd_timescale;
	mp4_clipper_parse_result_t result;
} process_moov_context_t;

typedef struct {
	// input
	request_context_t* request_context;
	media_parse_params_t parse_params;
	bool_t copy_data;

	size_t alloc_size;
	size_t stbl_atom_size;

	uint32_t timescale;
	uint32_t first_frame;
	uint32_t last_frame;
	uint32_t chunks;
	uint32_t first_chunk_index;
	uint32_t last_chunk_index;
	uint32_t first_chunk_frame_index;
	uint32_t last_chunk_frame_index;
	uint64_t first_frame_chunk_offset;
	uint64_t last_frame_chunk_offset;
} parse_trak_atom_context_t;

// constants
static const relevant_atom_t relevant_atoms_stbl[] = {
	{ ATOM_NAME_STSD, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STSD]), NULL },
	{ ATOM_NAME_STTS, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STTS]), NULL },
	{ ATOM_NAME_STSS, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STSS]), NULL },
	{ ATOM_NAME_CTTS, offsetof(parsed_trak_t, atoms[TRAK_ATOM_CTTS]), NULL },
	{ ATOM_NAME_STSC, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STSC]), NULL },
	{ ATOM_NAME_STSZ, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STSZ]), NULL },
	{ ATOM_NAME_STZ2, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STSZ]), NULL },
	{ ATOM_NAME_STCO, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STCO]), NULL },
	{ ATOM_NAME_CO64, offsetof(parsed_trak_t, atoms[TRAK_ATOM_STCO]), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_minf[] = {
	{ ATOM_NAME_STBL, 0, relevant_atoms_stbl },
	{ ATOM_NAME_DINF, offsetof(parsed_trak_t, atoms[TRAK_ATOM_DINF]), NULL },
	{ ATOM_NAME_VMHD, offsetof(parsed_trak_t, atoms[TRAK_ATOM_VMHD]), NULL },
	{ ATOM_NAME_SMHD, offsetof(parsed_trak_t, atoms[TRAK_ATOM_SMHD]), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_mdia[] = {
	{ ATOM_NAME_MINF, 0, relevant_atoms_minf },
	{ ATOM_NAME_HDLR, offsetof(parsed_trak_t, atoms[TRAK_ATOM_HDLR]), NULL },
	{ ATOM_NAME_MDHD, offsetof(parsed_trak_t, atoms[TRAK_ATOM_MDHD]), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static const relevant_atom_t relevant_atoms_trak[] = {
	{ ATOM_NAME_MDIA, 0, relevant_atoms_mdia },
	{ ATOM_NAME_TKHD, offsetof(parsed_trak_t, atoms[TRAK_ATOM_TKHD]), NULL },
	{ ATOM_NAME_NULL, 0, NULL }
};

static vod_str_t mp4_content_type = vod_string("video/mp4");

static void
mp4_clipper_write_tail(void* ctx, int index, void* buffer, uint32_t size)
{
	mp4_clipper_write_context* context = (mp4_clipper_write_context*)ctx;
	vod_chain_buf_t* elt;
	vod_buf_t* b;

	elt = &context->elts[index];

	b = &elt->b;
	b->pos = (u_char*)buffer;
	b->last = (u_char*)buffer + size;
	b->temporary = 1;

	elt->cl.buf = b;
	*context->last = &elt->cl;
	context->last = &elt->cl.next;
}


static vod_status_t
mp4_clipper_clip_duration(
	request_context_t* request_context, 
	media_parse_params_t* parse_params, 
	uint64_t* duration, 
	uint32_t timescale)
{
	uint64_t clip_from;
	uint64_t length;

	if (timescale == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_clipper_clip_duration: timescale is zero");
		return VOD_BAD_DATA;
	}

	clip_from = (uint64_t)parse_params->clip_from * timescale / 1000;
	if (*duration < clip_from)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_clipper_clip_duration: duration %uL less than clip from %uL", *duration, clip_from);
		return VOD_BAD_REQUEST;
	}

	(*duration) -= clip_from;

	if (parse_params->clip_to != UINT_MAX)
	{
		length = (uint64_t)(parse_params->clip_to - parse_params->clip_from) * timescale / 1000;
		if (*duration > length) 
		{
			*duration = length;
		}
	}

	return VOD_OK;
}

// mvhd
static vod_status_t
mp4_clipper_mvhd_clip_data(
	process_moov_context_t* context,
	atom_info_t* atom_info,
	mvhd_clip_result_t* result, 
	uint32_t* timescale)
{
	const mvhd_atom_t* atom = (const mvhd_atom_t*)atom_info->ptr;
	const mvhd64_atom_t* atom64 = (const mvhd64_atom_t*)atom_info->ptr;
	uint64_t duration;
	vod_status_t rc;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_clipper_mvhd_clip_data: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_clipper_mvhd_clip_data: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}

		*timescale = parse_be32(atom64->timescale);
		duration = parse_be64(atom64->duration);
	}
	else
	{
		*timescale = parse_be32(atom->timescale);
		duration = parse_be32(atom->duration);
	}

	rc = mp4_clipper_clip_duration(context->request_context, &context->parse_params, &duration, *timescale);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->atom = *atom_info;
	result->duration = duration;
	result->version = atom->version[0];

	context->result.moov_atom_size += full_atom_size(*atom_info);

	return VOD_OK;
}

static void
mp4_clipper_mvhd_update_atom(u_char* p, mvhd_clip_result_t* mvhd)
{
	mvhd64_atom_t* atom64;
	mvhd_atom_t* atom;

	if (mvhd->version == 1)
	{
		atom64 = (mvhd64_atom_t*)p;
		set_be64(atom64->duration, mvhd->duration);
	}
	else
	{
		atom = (mvhd_atom_t*)p;
		set_be32(atom->duration, mvhd->duration);
	}
}

// tkhd
static vod_status_t
mp4_clipper_tkhd_clip_data(
	process_moov_context_t* context,
	atom_info_t* atom_info,
	tkhd_clip_result_t* result)
{
	const tkhd_atom_t* atom = (const tkhd_atom_t*)atom_info->ptr;
	const tkhd64_atom_t* atom64 = (const tkhd64_atom_t*)atom_info->ptr;
	uint64_t duration;
	vod_status_t rc;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_clipper_tkhd_clip_data: atom size %uL too small (1)", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_clipper_tkhd_clip_data: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}

		duration = parse_be64(atom64->duration);
	}
	else
	{
		duration = parse_be32(atom->duration);
	}

	rc = mp4_clipper_clip_duration(context->request_context, &context->parse_params, &duration, context->mvhd_timescale);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->duration = duration;
	result->version = atom->version[0];

	return VOD_OK;
}

static void
mp4_clipper_tkhd_update_atom(u_char* p, tkhd_clip_result_t* tkhd)
{
	tkhd64_atom_t* atom64;
	tkhd_atom_t* atom;

	if (tkhd->version == 1)
	{
		atom64 = (tkhd64_atom_t*)p;
		set_be64(atom64->duration, tkhd->duration);
	}
	else
	{
		atom = (tkhd_atom_t*)p;
		set_be32(atom->duration, tkhd->duration);
	}
}

// mdhd
static vod_status_t
mp4_clipper_mdhd_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	mdhd_clip_result_t* result,
	uint32_t* timescale)
{
	const mdhd_atom_t* atom = (const mdhd_atom_t*)atom_info->ptr;
	const mdhd64_atom_t* atom64 = (const mdhd64_atom_t*)atom_info->ptr;
	uint64_t duration;
	vod_status_t rc;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_clipper_mdhd_clip_data: atom size %uL too small (1)", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom->version[0] == 1)
	{
		if (atom_info->size < sizeof(*atom64))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_clipper_mdhd_clip_data: atom size %uL too small (2)", atom_info->size);
			return VOD_BAD_DATA;
		}

		*timescale = parse_be32(atom64->timescale);
		duration = parse_be64(atom64->duration);
	}
	else
	{
		*timescale = parse_be32(atom->timescale);
		duration = parse_be32(atom->duration);
	}

	rc = mp4_clipper_clip_duration(context->request_context, &context->parse_params, &duration, *timescale);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->duration = duration;
	result->version = atom->version[0];

	return VOD_OK;
}

static void
mp4_clipper_mdhd_update_atom(u_char* p, mdhd_clip_result_t* mdhd)
{
	mdhd64_atom_t* atom64;
	mdhd_atom_t* atom;

	if (mdhd->version == 1)
	{
		atom64 = (mdhd64_atom_t*)p;
		set_be64(atom64->duration, mdhd->duration);
	}
	else
	{
		atom = (mdhd_atom_t*)p;
		set_be32(atom->duration, mdhd->duration);
	}
}

// stts
static vod_status_t
mp4_clipper_stts_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	stts_clip_result_t* result, 
	uint32_t* first_frame, 
	uint32_t* last_frame)
{
	stts_iterator_state_t iterator;
	vod_status_t rc;
	uint32_t entries;
	uint64_t clip_from;
	uint64_t clip_to;

	// validate the atom
	rc = mp4_parser_validate_stts_data(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (entries <= 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_clipper_stts_clip_data: zero entries");
		return VOD_BAD_DATA;
	}

	// parse the first sample
	mp4_parser_stts_iterator_init(
		&iterator,
		&context->parse_params,
		(stts_entry_t*)(atom_info->ptr + sizeof(stts_atom_t)),
		entries);

	if (context->parse_params.clip_from > 0)
	{
		clip_from = (((uint64_t)context->parse_params.clip_from * context->timescale) / 1000);
		if (!mp4_parser_stts_iterator(&iterator, clip_from))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_clipper_stts_clip_data: clip from longer than video duration");
			return VOD_BAD_REQUEST;
		}
	}

	result->first_entry = iterator.cur_entry;
	result->first_count = iterator.sample_count;
	*first_frame = iterator.frame_index;

	if (context->parse_params.clip_to != UINT_MAX)
	{
		// Note: the below was done to match nginx mp4, may be better to do 
		// clip_to = (((uint64_t)context->parse_params.clip_to * context->timescale) / 1000);
		clip_to = iterator.accum_duration + (((uint64_t)(context->parse_params.clip_to - context->parse_params.clip_from) * context->timescale) / 1000);
	}
	else
	{
		clip_to = ULLONG_MAX;
	}

	if (mp4_parser_stts_iterator(&iterator, clip_to))
	{
		result->last_entry = iterator.cur_entry + 1;
	}
	else
	{
		result->last_entry = iterator.cur_entry;
	}

	result->last_count = iterator.sample_count;
	*last_frame = iterator.frame_index;

	if (*first_frame >= *last_frame)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_clipper_stts_clip_data: no frames were found between clip from and clip to");
		return VOD_OK;
	}

	result->data_size = (u_char*)result->last_entry - (u_char*)result->first_entry;
	result->atom_size = ATOM_HEADER_SIZE + sizeof(stts_atom_t) + result->data_size;
	result->entries = result->last_entry - result->first_entry;

	context->stbl_atom_size += result->atom_size;
	context->alloc_size += ATOM_HEADER_SIZE + sizeof(stts_atom_t);

	return VOD_OK;
}

static u_char*
mp4_clipper_stts_write_atom(u_char* p, void* write_context, stts_clip_result_t* stts, bool_t copy_data)
{
	stts_entry_t* first_entry;
	stts_entry_t* last_entry;
	u_char* start = p;
	uint32_t last_count;

	write_be32(p, stts->atom_size);
	write_atom_name(p, 's', 't', 't', 's');
	write_be32(p, 0);
	write_be32(p, stts->entries);

	if (copy_data)
	{
		// copy
		first_entry = (stts_entry_t*)p;
		p = vod_copy(p, stts->first_entry, stts->data_size);
		last_entry = (stts_entry_t*)p;
	}
	else
	{
		// update in place
		first_entry = stts->first_entry;
		last_entry = stts->last_entry;

		// add to chain
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STTS_HEADER, start, p - start);
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STTS_DATA, first_entry, stts->data_size);
	}

	// update
	set_be32(first_entry->count, stts->first_count);
	last_count = parse_be32(last_entry[-1].count) - stts->last_count;
	set_be32(last_entry[-1].count, last_count);

	return p;
}

static vod_status_t
mp4_clipper_stss_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	stss_clip_result_t* result)
{
	uint32_t* start_pos;
	uint32_t entries;
	vod_status_t rc;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}

	rc = mp4_parser_validate_stss_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	start_pos = (uint32_t*)(atom_info->ptr + sizeof(stss_atom_t));

	result->first_entry = start_pos;
	if (context->first_frame > 0)
	{
		result->first_entry += mp4_parser_find_stss_entry(context->first_frame, start_pos, entries);
	}
	if (context->last_frame != UINT_MAX)
	{
		result->last_entry = start_pos + mp4_parser_find_stss_entry(context->last_frame, start_pos, entries);
	}
	else
	{
		result->last_entry = start_pos + entries;
	}

	if (result->first_entry < result->last_entry)
	{
		result->data_size = (u_char*)result->last_entry - (u_char*)result->first_entry;
		result->entries = result->last_entry - result->first_entry;
	}
	else
	{
		result->data_size = 0;
		result->entries = 0;
	}

	result->first_frame = context->first_frame;

	result->atom_size = ATOM_HEADER_SIZE + sizeof(stss_atom_t) + result->data_size;

	context->stbl_atom_size += result->atom_size;
	context->alloc_size += ATOM_HEADER_SIZE + sizeof(stss_atom_t);

	return VOD_OK;
}

static u_char*
mp4_clipper_stss_write_atom(u_char* p, void* write_context, stss_clip_result_t* stss, bool_t copy_data)
{
	u_char* start = p;
	uint32_t* cur_entry;
	uint32_t frame_index;

	if (stss->atom_size == 0)
	{
		return p;
	}

	write_be32(p, stss->atom_size);
	write_atom_name(p, 's', 't', 's', 's');
	write_be32(p, 0);
	write_be32(p, stss->entries);

	if (copy_data)
	{
		for (cur_entry = stss->first_entry; cur_entry < stss->last_entry; cur_entry++)
		{
			frame_index = parse_be32(cur_entry) - stss->first_frame;
			write_be32(p, frame_index);
		}
	}
	else
	{
		// update in place
		for (cur_entry = stss->first_entry; cur_entry < stss->last_entry; cur_entry++)
		{
			frame_index = parse_be32(cur_entry) - stss->first_frame;
			set_be32(cur_entry, frame_index);
		}

		// add to chain
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSS_HEADER, start, p - start);
		if (stss->data_size > 0)
		{
			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSS_DATA, stss->first_entry, stss->data_size);
		}
	}

	return p;
}

static vod_status_t
mp4_clipper_ctts_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	ctts_clip_result_t* result)
{
	ctts_iterator_state_t iterator;
	uint32_t entries;
	vod_status_t rc;

	if (atom_info->size == 0)		// optional atom
	{
		return VOD_OK;
	}

	// validate the atom
	rc = mp4_parser_validate_ctts_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// parse the first sample
	mp4_parser_ctts_iterator_init(
		&iterator,
		(ctts_entry_t*)(atom_info->ptr + sizeof(ctts_atom_t)),
		entries);

	if (context->first_frame > 0)
	{
		if (!mp4_parser_ctts_iterator(&iterator, context->first_frame))
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mp4_clipper_ctts_clip_data: failed to find first frame");
			return VOD_BAD_DATA;
		}
	}

	result->first_entry = iterator.cur_entry;
	result->first_count = iterator.sample_count;

	if (context->parse_params.clip_to != UINT_MAX && 
		mp4_parser_ctts_iterator(&iterator, context->last_frame))
	{
		result->last_entry = iterator.cur_entry + 1;
		result->last_count = iterator.sample_count;
	}
	else
	{
		result->last_entry = iterator.last_entry;
		result->last_count = 0;
	}

	result->data_size = (u_char*)result->last_entry - (u_char*)result->first_entry;
	result->atom_size = ATOM_HEADER_SIZE + sizeof(ctts_atom_t) + result->data_size;
	result->entries = result->last_entry - result->first_entry;

	context->stbl_atom_size += result->atom_size;
	context->alloc_size += ATOM_HEADER_SIZE + sizeof(ctts_atom_t);

	return VOD_OK;
}

static u_char*
mp4_clipper_ctts_write_atom(u_char* p, void* write_context, ctts_clip_result_t* ctts, bool_t copy_data)
{
	ctts_entry_t* first_entry;
	ctts_entry_t* last_entry;
	u_char* start = p;
	uint32_t last_count;

	if (ctts->atom_size == 0)
	{
		return p;
	}

	write_be32(p, ctts->atom_size);
	write_atom_name(p, 'c', 't', 't', 's');
	write_be32(p, 0);
	write_be32(p, ctts->entries);

	if (copy_data)
	{
		// copy
		first_entry = (ctts_entry_t*)p;
		p = vod_copy(p, ctts->first_entry, ctts->data_size);
		last_entry = (ctts_entry_t*)p;
	}
	else
	{
		// update in place
		first_entry = ctts->first_entry;
		last_entry = ctts->last_entry;

		// add to chain
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_CTTS_HEADER, start, p - start);
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_CTTS_DATA, ctts->first_entry, ctts->data_size);
	}

	// update
	set_be32(first_entry->count, ctts->first_count);
	last_count = parse_be32(last_entry[-1].count) - ctts->last_count;
	set_be32(last_entry[-1].count, last_count);

	return p;
}

static vod_status_t
mp4_clipper_stsc_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	stsc_clip_result_t* result,
	uint32_t* first_chunk_frame_index,
	uint32_t* last_chunk_frame_index)
{
	stsc_iterator_state_t iterator;
	bool_t single_chunk = 0;
	uint32_t last_entry_samples_per_chunk;
	uint32_t last_entry_first_chunk;
	uint32_t target_chunk;
	uint32_t sample_count;
	uint32_t prev_samples;
	uint32_t next_chunk;
	uint32_t entries;
	vod_status_t rc;

	rc = mp4_parser_validate_stsc_atom(context->request_context, atom_info, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// init the iterator
	rc = mp4_parser_stsc_iterator_init(
		&iterator,
		context->request_context,
		(stsc_entry_t*)(atom_info->ptr + sizeof(stsc_atom_t)),
		entries, 
		context->chunks);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// jump to the first frame
	rc = mp4_parser_stsc_iterator(&iterator, context->first_frame, &target_chunk, &sample_count, &next_chunk, &prev_samples);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->first_entry = iterator.cur_entry;
	result->first_sample_count = sample_count;
	result->first_entry_sample_desc = iterator.sample_desc;
	result->first_entry_samples_per_chunk = iterator.samples_per_chunk - sample_count;
	result->first_chunk = target_chunk;
	result->pre_entry = (result->first_sample_count && next_chunk - target_chunk != 2);

	// jump to the last frame
	rc = mp4_parser_stsc_iterator(&iterator, context->last_frame, &target_chunk, &sample_count, &next_chunk, &prev_samples);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->last_entry = iterator.cur_entry;
	result->last_sample_count = sample_count;
	result->last_entry_sample_desc = iterator.sample_desc;

	// get the first chunk of the last entry
	if (result->first_entry == result->last_entry)
	{
		if (result->pre_entry)
		{
			last_entry_first_chunk = result->first_chunk + 2;
		}
		else
		{
			last_entry_first_chunk = result->first_chunk + 1;
		}
	}
	else
	{
		last_entry_first_chunk = iterator.cur_chunk;
	}

	// special handling for the case in which the pre entry has all the frames
	if (result->pre_entry &&
		result->first_entry_samples_per_chunk >= context->last_frame - context->first_frame)
	{
		result->first_entry_samples_per_chunk = context->last_frame - context->first_frame;

		single_chunk = 1;
	}

	// output the last entry if used
	if (context->last_frame > iterator.frame_index && !single_chunk)
	{
		result->last_entry++;
	}

	// calculate last chunk and last entry samples
	if (result->last_sample_count) 
	{
		result->last_chunk = target_chunk + 1;

		last_entry_samples_per_chunk = result->last_sample_count;

		if (single_chunk)
		{
			result->last_sample_count = 0;
		}
		else if (target_chunk == result->first_chunk)
		{
			result->last_sample_count -= result->first_sample_count;
		}
	}
	else 
	{
		result->last_chunk = target_chunk;

		if (context->last_frame > iterator.frame_index)
		{
			last_entry_samples_per_chunk = iterator.samples_per_chunk;
		}
		else
		{
			last_entry_samples_per_chunk = prev_samples;
		}
	}

	result->post_entry = (result->last_sample_count && last_entry_first_chunk != result->last_chunk);

	*first_chunk_frame_index = context->first_frame - result->first_sample_count;
	*last_chunk_frame_index = context->last_frame - last_entry_samples_per_chunk;

	// get number of entries and update alloc size
	result->entries = result->last_entry - result->first_entry;
	context->alloc_size += ATOM_HEADER_SIZE + sizeof(stsc_atom_t);
	if (result->pre_entry)
	{
		result->entries++;
		context->alloc_size += sizeof(stsc_entry_t);
	}
	if (result->post_entry)
	{
		result->entries++;
		context->alloc_size += sizeof(stsc_entry_t);
	}

	// calculate atom size
	result->atom_size =
		ATOM_HEADER_SIZE +
		sizeof(stsc_atom_t)+
		result->entries * sizeof(stsc_entry_t);

	context->stbl_atom_size += result->atom_size;

	return VOD_OK;
}

static u_char*
mp4_clipper_stsc_write_atom(u_char* p, void* write_context, stsc_clip_result_t* stsc, bool_t copy_data)
{
	stsc_entry_t* cur_entry;
	uint32_t chunk_diff;
	uint32_t first_chunk;
	stsc_entry_t* first_entry;
	stsc_entry_t* last_entry;
	u_char* start = p;

	write_be32(p, stsc->atom_size);
	write_atom_name(p, 's', 't', 's', 'c');
	write_be32(p, 0);
	write_be32(p, stsc->entries);

	// add an preceding entry if needed
	if (stsc->pre_entry)
	{
		write_be32(p, 1);
		write_be32(p, stsc->first_entry_samples_per_chunk);
		write_be32(p, stsc->first_entry_sample_desc);
	}

	// get writable middle section
	if (!copy_data)
	{
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSC_START, start, p - start);
		start = p;

		first_entry = stsc->first_entry;
		last_entry = stsc->last_entry;
	}
	else
	{
		first_entry = (stsc_entry_t*)p;
		p = vod_copy(p, stsc->first_entry, (u_char*)stsc->last_entry - (u_char*)stsc->first_entry);
		last_entry = (stsc_entry_t*)p;
	}

	// fix first / last samples per chunk
	if (stsc->first_sample_count && !stsc->pre_entry)
	{
		set_be32(first_entry[0].samples_per_chunk, stsc->first_entry_samples_per_chunk);
	}

	if (stsc->last_sample_count && !stsc->post_entry)
	{
		set_be32(last_entry[-1].samples_per_chunk, stsc->last_sample_count);
	}

	// fix first chunks
	if (stsc->pre_entry)
	{
		set_be32(first_entry->first_chunk, 2);
	}
	else
	{
		set_be32(first_entry->first_chunk, 1);
	}

	chunk_diff = stsc->first_chunk;
	for (cur_entry = first_entry + 1; cur_entry < last_entry; cur_entry++)
	{
		first_chunk = parse_be32(cur_entry->first_chunk) - chunk_diff;
		set_be32(cur_entry->first_chunk, first_chunk);
	}

	if (!copy_data)
	{
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSC_DATA, first_entry, (u_char*)last_entry - (u_char*)first_entry);
		start = p;
	}

	// add a trailing entry if needed
	if (stsc->post_entry)
	{
		write_be32(p, stsc->last_chunk - stsc->first_chunk);
		write_be32(p, stsc->last_sample_count);
		write_be32(p, stsc->last_entry_sample_desc);

		if (!copy_data)
		{
			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSC_END, start, p - start);
		}
	}

	return p;
}

static vod_status_t
mp4_clipper_stsz_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	stsz_clip_result_t* result,
	uint64_t* first_frame_chunk_offset,
	uint64_t* last_frame_chunk_offset)
{
	uint32_t first_frame_index_in_chunk = context->first_frame - context->first_chunk_frame_index;
	uint32_t last_frame_index_in_chunk = context->last_frame - context->last_chunk_frame_index;
	uint32_t uniform_size;
	uint32_t entries;
	uint32_t field_size;
	const u_char* cur_pos;
	const u_char* data_start;
	vod_status_t rc;

	rc = mp4_parser_validate_stsz_atom(context->request_context, atom_info, context->last_frame, &uniform_size, &field_size, &entries);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->uniform_size = uniform_size;
	result->field_size = field_size;

	if (uniform_size != 0)
	{
		result->first_entry = NULL;
		result->entries = 0;
		result->data_size = 0;
		result->atom_size = ATOM_HEADER_SIZE + sizeof(stsz_atom_t);
		*first_frame_chunk_offset = ((uint64_t)first_frame_index_in_chunk) * uniform_size;
		*last_frame_chunk_offset = ((uint64_t)last_frame_index_in_chunk) * uniform_size;

		context->stbl_atom_size += result->atom_size;
		context->alloc_size += ATOM_HEADER_SIZE + sizeof(stsz_atom_t);

		return VOD_OK;
	}

	*first_frame_chunk_offset = 0;
	*last_frame_chunk_offset = 0;
	data_start = atom_info->ptr + sizeof(stsz_atom_t);

	switch (field_size)
	{
	case 32:
		cur_pos = data_start + context->first_chunk_frame_index * sizeof(uint32_t);
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--, cur_pos += sizeof(uint32_t))
		{
			(*first_frame_chunk_offset) += parse_be32(cur_pos);
		}

		cur_pos = data_start + context->last_chunk_frame_index * sizeof(uint32_t);
		for (; last_frame_index_in_chunk; last_frame_index_in_chunk--, cur_pos += sizeof(uint32_t))
		{
			(*last_frame_chunk_offset) += parse_be32(cur_pos);
		}
		break;

	case 16:
		cur_pos = data_start + context->first_chunk_frame_index * sizeof(uint16_t);
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--, cur_pos += sizeof(uint16_t))
		{
			(*first_frame_chunk_offset) += parse_be16(cur_pos);
		}

		cur_pos = data_start + context->last_chunk_frame_index * sizeof(uint16_t);
		for (; last_frame_index_in_chunk; last_frame_index_in_chunk--, cur_pos += sizeof(uint16_t))
		{
			(*last_frame_chunk_offset) += parse_be16(cur_pos);
		}
		break;

	case 8:
		cur_pos = data_start + context->first_chunk_frame_index;
		for (; first_frame_index_in_chunk; first_frame_index_in_chunk--)
		{
			(*first_frame_chunk_offset) += *cur_pos++;
		}

		cur_pos = data_start + context->last_chunk_frame_index;
		for (; last_frame_index_in_chunk; last_frame_index_in_chunk--)
		{
			(*last_frame_chunk_offset) += *cur_pos++;
		}
		break;

	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_clipper_stsz_clip_data: unsupported field size %uD", field_size);
		return VOD_BAD_DATA;
	}

	result->first_entry = (u_char*)data_start + context->first_frame * (field_size >> 3);
	result->entries = context->last_frame - context->first_frame;
	result->data_size = result->entries * (field_size >> 3);
	result->atom_size = ATOM_HEADER_SIZE + sizeof(stsz_atom_t) + result->data_size;

	context->stbl_atom_size += result->atom_size;
	context->alloc_size += ATOM_HEADER_SIZE + sizeof(stsz_atom_t);

	return VOD_OK;
}

static u_char*
mp4_clipper_stsz_write_atom(u_char* p, void* write_context, stsz_clip_result_t* stsz, bool_t copy_data)
{
	u_char* start = p;

	if (stsz->field_size == 32 || stsz->uniform_size != 0)
	{
		write_be32(p, stsz->atom_size);
		write_atom_name(p, 's', 't', 's', 'z');
		write_be32(p, 0);
		write_be32(p, stsz->uniform_size);
		write_be32(p, stsz->entries);
	}
	else
	{
		write_be32(p, stsz->atom_size);
		write_atom_name(p, 's', 't', 'z', '2');
		write_be32(p, 0);
		write_be32(p, stsz->field_size);
		write_be32(p, stsz->entries);
	}

	if (copy_data)
	{
		p = vod_copy(p, stsz->first_entry, stsz->data_size);
	}
	else
	{
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSZ_HEADER, start, p - start);
		if (stsz->data_size > 0)
		{
			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STSZ_DATA, stsz->first_entry, stsz->data_size);
		}
	}

	return p;
}

static vod_status_t 
mp4_clipper_stco_init_chunk_count(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info)
{
	stco_atom_t* stco_atom;

	if (atom_info->size < sizeof(*stco_atom))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mp4_clipper_stco_init_chunk_count: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	stco_atom = (stco_atom_t*)atom_info->ptr;
	context->chunks = parse_be32(stco_atom->entries);

	return VOD_OK;
}

static vod_status_t
mp4_clipper_stco_clip_data(
	parse_trak_atom_context_t* context,
	atom_info_t* atom_info,
	stco_clip_result_t* result,
	uint64_t* first_offset,
	uint64_t* last_offset)
{
	uint32_t entries;
	uint32_t entry_size;
	vod_status_t rc;
	u_char* last_entry;

	rc = mp4_parser_validate_stco_data(
		context->request_context,
		atom_info,
		context->last_chunk_index,
		&entries,
		&entry_size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	result->entry_size = entry_size;
	result->first_entry = (u_char*)atom_info->ptr + sizeof(stco_atom_t) + context->first_chunk_index * entry_size;
	result->last_entry = (u_char*)atom_info->ptr + sizeof(stco_atom_t) + context->last_chunk_index * entry_size;

	if (atom_info->name == ATOM_NAME_CO64)
	{
		*first_offset = parse_be64(result->first_entry);
		last_entry = result->last_entry - sizeof(uint64_t);
		*last_offset = parse_be64(last_entry);
	}
	else
	{
		*first_offset = parse_be32(result->first_entry);
		last_entry = result->last_entry - sizeof(uint32_t);
		*last_offset = parse_be32(last_entry);
	}
	(*first_offset) += context->first_frame_chunk_offset;
	(*last_offset) += context->last_frame_chunk_offset;

	result->first_frame_chunk_offset = context->first_frame_chunk_offset;

	result->entries = context->last_chunk_index - context->first_chunk_index;
	result->data_size = result->last_entry - result->first_entry;
	result->atom_size = ATOM_HEADER_SIZE + sizeof(stco_atom_t) + result->data_size;

	context->stbl_atom_size += result->atom_size;
	context->alloc_size += ATOM_HEADER_SIZE + sizeof(stco_atom_t);

	return VOD_OK;
}

static u_char*
mp4_clipper_stco_write_atom(u_char* p, void* write_context, stco_clip_result_t* stco, bool_t copy_data, int64_t chunk_pos_diff)
{
	uint64_t chunk_offset;
	u_char* start = p;
	u_char* cur_pos;

	if (stco->entry_size == sizeof(uint32_t))
	{
		write_be32(p, stco->atom_size);
		write_atom_name(p, 's', 't', 'c', 'o');
		write_be32(p, 0);
		write_be32(p, stco->entries);

		if (copy_data)
		{
			chunk_offset = parse_be32(stco->first_entry) - chunk_pos_diff + stco->first_frame_chunk_offset;
			write_be32(p, chunk_offset);

			for (cur_pos = stco->first_entry + sizeof(uint32_t); cur_pos < stco->last_entry; cur_pos += sizeof(uint32_t))
			{
				chunk_offset = parse_be32(cur_pos) - chunk_pos_diff;
				write_be32(p, chunk_offset);
			}
		}
		else
		{
			chunk_offset = parse_be32(stco->first_entry) - chunk_pos_diff + stco->first_frame_chunk_offset;
			set_be32(stco->first_entry, chunk_offset);

			for (cur_pos = stco->first_entry + sizeof(uint32_t); cur_pos < stco->last_entry; cur_pos += sizeof(uint32_t))
			{
				chunk_offset = parse_be32(cur_pos) - chunk_pos_diff;
				set_be32(cur_pos, chunk_offset);
			}

			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STCO_HEADER, start, p - start);
			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STCO_DATA, stco->first_entry, stco->data_size);
		}
	}
	else
	{
		write_be32(p, stco->atom_size);
		write_atom_name(p, 'c', 'o', '6', '4');
		write_be32(p, 0);
		write_be32(p, stco->entries);

		if (copy_data)
		{
			chunk_offset = parse_be64(stco->first_entry) - chunk_pos_diff + stco->first_frame_chunk_offset;
			write_be64(p, chunk_offset);

			for (cur_pos = stco->first_entry + sizeof(uint64_t); cur_pos < stco->last_entry; cur_pos += sizeof(uint64_t))
			{
				chunk_offset = parse_be64(cur_pos) - chunk_pos_diff;
				write_be64(p, chunk_offset);
			}
		}
		else
		{
			chunk_offset = parse_be64(stco->first_entry) - chunk_pos_diff + stco->first_frame_chunk_offset;
			set_be64(stco->first_entry, chunk_offset);

			for (cur_pos = stco->first_entry + sizeof(uint64_t); cur_pos < stco->last_entry; cur_pos += sizeof(uint64_t))
			{
				chunk_offset = parse_be64(cur_pos) - chunk_pos_diff;
				set_be64(cur_pos, chunk_offset);
			}

			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STCO_HEADER, start, p - start);
			mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STCO_DATA, stco->first_entry, stco->data_size);
		}
	}

	return p;
}

static vod_status_t
mp4_clipper_get_media_type(
	request_context_t* request_context, 
	atom_info_t* hdlr_atom_info, 
	uint32_t* media_type)
{
	const hdlr_atom_t* atom = (const hdlr_atom_t*)hdlr_atom_info->ptr;
	uint32_t type;

	if (hdlr_atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_clipper_get_media_type: hdlr atom size %uL too small", hdlr_atom_info->size);
		return VOD_BAD_DATA;
	}

	type = parse_le32(atom->type);
	switch (type)
	{
	case HDLR_TYPE_VIDE:
		*media_type = MEDIA_TYPE_VIDEO;
		break;

	case HDLR_TYPE_SOUN:
		*media_type = MEDIA_TYPE_AUDIO;
		break;

	default:
		*media_type = MEDIA_TYPE_NONE;
		break;
	}

	return VOD_OK;
}

static vod_status_t
mp4_clipper_process_moov_atom_callback(void* ctx, atom_info_t* atom_info)
{
	process_moov_context_t* context = (process_moov_context_t*)ctx;
	save_relevant_atoms_context_t save_atoms_context;
	parsed_trak_t** result;
	parsed_trak_t* parsed_trak;
	vod_status_t rc;
	uint64_t first_offset = 0;
	uint64_t last_offset = 0;
	uint32_t track_index;
	uint32_t media_type;

	switch (atom_info->name)
	{
	case ATOM_NAME_MVHD:
		rc = mp4_clipper_mvhd_clip_data(context, atom_info, &context->result.mvhd, &context->mvhd_timescale);
		if (rc != VOD_OK)
		{
			return rc;
		}

		return VOD_OK;

	case ATOM_NAME_TRAK:
		break;

	default:				// ignore
		return VOD_OK;
	}

	parsed_trak = vod_alloc(context->request_context->pool, sizeof(*parsed_trak));
	if (parsed_trak == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_clipper_process_moov_atom_callback: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memzero(parsed_trak, sizeof(*parsed_trak));

	// find required trak atoms
	save_atoms_context.relevant_atoms = relevant_atoms_trak;
	save_atoms_context.result = parsed_trak;
	save_atoms_context.request_context = context->request_context;
	rc = mp4_parser_parse_atoms(context->request_context, atom_info->ptr, atom_info->size, TRUE, &mp4_parser_save_relevant_atoms_callback, &save_atoms_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// check whether we should include this track
	rc = mp4_clipper_get_media_type(
		context->request_context,
		&parsed_trak->atoms[TRAK_ATOM_HDLR],
		&media_type);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (media_type != MEDIA_TYPE_NONE)
	{
		track_index = context->track_indexes[media_type]++;
		if (!vod_is_bit_set(context->parse_params.required_tracks_mask[media_type], track_index))
		{
			return VOD_OK;
		}
	}

	parse_trak_atom_context_t parse_context;
	parse_context.request_context = context->request_context;
	parse_context.parse_params = context->parse_params;
	parse_context.stbl_atom_size = ATOM_HEADER_SIZE + full_atom_size(parsed_trak->atoms[TRAK_ATOM_STSD]);
	parse_context.alloc_size = 4 * ATOM_HEADER_SIZE;	// trak, mdia, minf, stbl
	parse_context.copy_data = context->result.copy_data;

	rc = mp4_clipper_tkhd_clip_data(context, &parsed_trak->atoms[TRAK_ATOM_TKHD], &parsed_trak->tkhd);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = mp4_clipper_mdhd_clip_data(
		&parse_context, 
		&parsed_trak->atoms[TRAK_ATOM_MDHD], 
		&parsed_trak->mdhd, 
		&parse_context.timescale);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = mp4_clipper_stts_clip_data(
		&parse_context, 
		&parsed_trak->atoms[TRAK_ATOM_STTS], 
		&parsed_trak->stts, 
		&parse_context.first_frame, 
		&parse_context.last_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (parse_context.first_frame >= parse_context.last_frame)
	{
		return VOD_OK;
	}

	rc = mp4_clipper_stss_clip_data(&parse_context, &parsed_trak->atoms[TRAK_ATOM_STSS], &parsed_trak->stss);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = mp4_clipper_ctts_clip_data(&parse_context, &parsed_trak->atoms[TRAK_ATOM_CTTS], &parsed_trak->ctts);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = mp4_clipper_stco_init_chunk_count(&parse_context, &parsed_trak->atoms[TRAK_ATOM_STCO]);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	rc = mp4_clipper_stsc_clip_data(
		&parse_context, 
		&parsed_trak->atoms[TRAK_ATOM_STSC], 
		&parsed_trak->stsc, 
		&parse_context.first_chunk_frame_index, 
		&parse_context.last_chunk_frame_index);
	if (rc != VOD_OK)
	{
		return rc;
	}

	parse_context.first_chunk_index = parsed_trak->stsc.first_chunk;
	parse_context.last_chunk_index = parsed_trak->stsc.last_chunk;

	rc = mp4_clipper_stsz_clip_data(
		&parse_context, 
		&parsed_trak->atoms[TRAK_ATOM_STSZ], 
		&parsed_trak->stsz,
		&parse_context.first_frame_chunk_offset,
		&parse_context.last_frame_chunk_offset);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = mp4_clipper_stco_clip_data(
		&parse_context, 
		&parsed_trak->atoms[TRAK_ATOM_STCO], 
		&parsed_trak->stco, 
		&first_offset,
		&last_offset);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (first_offset < context->result.base.first_offset)
	{
		context->result.base.first_offset = first_offset;
	}

	if (last_offset > context->result.base.last_offset)
	{
		context->result.base.last_offset = last_offset;
	}

	parsed_trak->stbl_atom_size = parse_context.stbl_atom_size;
	
	parsed_trak->minf_atom_size = ATOM_HEADER_SIZE +
		full_atom_size(parsed_trak->atoms[TRAK_ATOM_VMHD]) +
		full_atom_size(parsed_trak->atoms[TRAK_ATOM_SMHD]) +
		full_atom_size(parsed_trak->atoms[TRAK_ATOM_DINF]) +
		parsed_trak->stbl_atom_size;
	
	parsed_trak->mdia_atom_size = ATOM_HEADER_SIZE +
		full_atom_size(parsed_trak->atoms[TRAK_ATOM_MDHD]) +
		full_atom_size(parsed_trak->atoms[TRAK_ATOM_HDLR]) +
		parsed_trak->minf_atom_size;

	parsed_trak->trak_atom_size = ATOM_HEADER_SIZE +
		full_atom_size(parsed_trak->atoms[TRAK_ATOM_TKHD]) +
		parsed_trak->mdia_atom_size;

	context->result.moov_atom_size += parsed_trak->trak_atom_size;

	context->result.alloc_size += parse_context.alloc_size;

	result = vod_array_push(&context->result.parsed_traks);
	if (result == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mp4_clipper_process_moov_atom_callback: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}

	*result = parsed_trak;

	return VOD_OK;
}

static u_char*
mp4_clipper_trak_write_atom(u_char* p, void* write_context, parsed_trak_t* parsed_trak, bool_t copy_data, int64_t chunk_pos_diff)
{
	if (copy_data)
	{
		write_atom_header(p, parsed_trak->trak_atom_size, 't', 'r', 'a', 'k');
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_TKHD]);
		mp4_clipper_tkhd_update_atom(p - parsed_trak->atoms[TRAK_ATOM_TKHD].size, &parsed_trak->tkhd);
		write_atom_header(p, parsed_trak->mdia_atom_size, 'm', 'd', 'i', 'a');
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_MDHD]);
		mp4_clipper_mdhd_update_atom(p - parsed_trak->atoms[TRAK_ATOM_MDHD].size, &parsed_trak->mdhd);
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_HDLR]);
		write_atom_header(p, parsed_trak->minf_atom_size, 'm', 'i', 'n', 'f');
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_VMHD]);
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_SMHD]);
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_DINF]);
		write_atom_header(p, parsed_trak->stbl_atom_size, 's', 't', 'b', 'l');
		copy_full_atom(p, parsed_trak->atoms[TRAK_ATOM_STSD]);
	}
	else
	{
		// trak
		write_atom_header(p, parsed_trak->trak_atom_size, 't', 'r', 'a', 'k');
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_HEADER, p - ATOM_HEADER_SIZE, ATOM_HEADER_SIZE);

		// trak.tkhd
		mp4_clipper_tkhd_update_atom((u_char*)parsed_trak->atoms[TRAK_ATOM_TKHD].ptr, &parsed_trak->tkhd);
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_TKHD_ATOM, parsed_trak->atoms[TRAK_ATOM_TKHD]);

		// trak.mdia
		write_atom_header(p, parsed_trak->mdia_atom_size, 'm', 'd', 'i', 'a');
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_MDIA_HEADER, p - ATOM_HEADER_SIZE, ATOM_HEADER_SIZE);

		// trak.mdia.*
		mp4_clipper_mdhd_update_atom((u_char*)parsed_trak->atoms[TRAK_ATOM_MDHD].ptr, &parsed_trak->mdhd);
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_MDHD_ATOM, parsed_trak->atoms[TRAK_ATOM_MDHD]);
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_HDLR_ATOM, parsed_trak->atoms[TRAK_ATOM_HDLR]);

		// trak.mdia.minf
		write_atom_header(p, parsed_trak->minf_atom_size, 'm', 'i', 'n', 'f');
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_MINF_HEADER, p - ATOM_HEADER_SIZE, ATOM_HEADER_SIZE);

		// trak.mdia.minf.*
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_VMHD_ATOM, parsed_trak->atoms[TRAK_ATOM_VMHD]);
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_SMHD_ATOM, parsed_trak->atoms[TRAK_ATOM_SMHD]);
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_DINF_ATOM, parsed_trak->atoms[TRAK_ATOM_DINF]);

		// trak.mdia.minf.stbl
		write_atom_header(p, parsed_trak->stbl_atom_size, 's', 't', 'b', 'l');
		mp4_clipper_write_tail(write_context, MP4_CLIPPER_TRAK_INDEX_STBL_HEADER, p - ATOM_HEADER_SIZE, ATOM_HEADER_SIZE);

		// trak.mdia.minf.stbl.stsd
		write_full_atom(write_context, MP4_CLIPPER_TRAK_INDEX_STSD_ATOM, parsed_trak->atoms[TRAK_ATOM_STSD]);
	}

	p = mp4_clipper_stts_write_atom(p, write_context, &parsed_trak->stts, copy_data);
	p = mp4_clipper_stss_write_atom(p, write_context, &parsed_trak->stss, copy_data);
	p = mp4_clipper_ctts_write_atom(p, write_context, &parsed_trak->ctts, copy_data);
	p = mp4_clipper_stsc_write_atom(p, write_context, &parsed_trak->stsc, copy_data);
	p = mp4_clipper_stsz_write_atom(p, write_context, &parsed_trak->stsz, copy_data);
	p = mp4_clipper_stco_write_atom(p, write_context, &parsed_trak->stco, copy_data, chunk_pos_diff);

	return p;
}

vod_status_t 
mp4_clipper_parse_moov(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	bool_t copy_data,
	media_clipper_parse_result_t** result)
{
	process_moov_context_t process_moov_context;
	mp4_clipper_parse_result_t* parse_result;
	vod_status_t rc;

	vod_memzero(&process_moov_context, sizeof(process_moov_context));
	if (vod_array_init(&process_moov_context.result.parsed_traks, request_context->pool, 2, sizeof(parsed_trak_t*)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_clipper_parse_moov: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	process_moov_context.request_context = request_context;
	process_moov_context.parse_params = *parse_params;
	vod_memzero(process_moov_context.track_indexes, sizeof(process_moov_context.track_indexes));
	process_moov_context.result.copy_data = copy_data;
	process_moov_context.result.moov_atom_size = ATOM_HEADER_SIZE;
	process_moov_context.result.alloc_size = ATOM_HEADER_SIZE;		// moov
	process_moov_context.result.base.first_offset = ULLONG_MAX;

	rc = mp4_parser_parse_atoms(
		request_context, 
		metadata_parts[MP4_METADATA_PART_MOOV].data, 
		metadata_parts[MP4_METADATA_PART_MOOV].len,
		TRUE, 
		&mp4_clipper_process_moov_atom_callback, 
		&process_moov_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (copy_data)
	{
		process_moov_context.result.alloc_size = process_moov_context.result.moov_atom_size;
	}

	parse_result = vod_alloc(request_context->pool, sizeof(process_moov_context.result));
	if (parse_result == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_clipper_parse_moov: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	*parse_result = process_moov_context.result;

	*result = &parse_result->base;

	return VOD_OK;
}

vod_status_t
mp4_clipper_build_header(
	request_context_t* request_context,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	media_clipper_parse_result_t* parse_result,
	vod_chain_t** result,
	size_t* response_size,
	vod_str_t* content_type)
{
	mp4_clipper_parse_result_t* mp4_parse_result = vod_container_of(parse_result, mp4_clipper_parse_result_t, base);
	mp4_clipper_write_context write_context;
	vod_chain_buf_t* write_elts;
	parsed_trak_t** cur_trak;
	parsed_trak_t** last_trak;
	uint64_t mdat_atom_size;
	int64_t chunk_pos_diff;
	size_t ftyp_header_size;
	size_t ftyp_atom_size;
	size_t ftyp_size = metadata_parts[MP4_METADATA_PART_FTYP].len;
	size_t mdat_header_size;
	size_t buffer_count;
	size_t alloc_size;
	u_char* output;
	u_char* p;

	// init the buffers and chains
	if (mp4_parse_result->copy_data)
	{
		buffer_count = 1;
	}
	else
	{
		buffer_count = MP4_CLIPPER_INDEX_COUNT + MP4_CLIPPER_TRAK_INDEX_COUNT * mp4_parse_result->parsed_traks.nelts;
	}
	alloc_size = buffer_count * sizeof(write_elts[0]);
	write_elts = vod_alloc(request_context->pool, alloc_size);
	if (write_elts == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_clipper_build_header: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}
	vod_memzero(write_elts, alloc_size);

	write_context.elts = write_elts;
	write_context.last = result;

	// calculate ftyp size
	ftyp_header_size = ftyp_size > 0 ? ATOM_HEADER_SIZE : 0;
	ftyp_atom_size = ftyp_header_size + ftyp_size;

	// calculate the mdat size and chunk offset
	mdat_atom_size = ATOM_HEADER_SIZE + parse_result->last_offset - parse_result->first_offset;
	if (mdat_atom_size > (uint64_t)0xffffffff)
	{
		mdat_header_size = ATOM_HEADER64_SIZE;
		mdat_atom_size += ATOM_HEADER64_SIZE - ATOM_HEADER_SIZE;
	}
	else
	{
		mdat_header_size = ATOM_HEADER_SIZE;
	}
	chunk_pos_diff = parse_result->first_offset - (ftyp_atom_size + mp4_parse_result->moov_atom_size + mdat_header_size);

	// allocate the data buffer
	mp4_parse_result->alloc_size += ftyp_header_size + mdat_header_size;
	if (mp4_parse_result->copy_data)
	{
		mp4_parse_result->alloc_size += ftyp_size;
	}

	output = vod_alloc(request_context->pool, mp4_parse_result->alloc_size);
	if (output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_clipper_build_header: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	p = output;

	// ftyp
	if (ftyp_size > 0)
	{
		write_atom_header(p, ftyp_atom_size, 'f', 't', 'y', 'p');
		if (mp4_parse_result->copy_data)
		{
			p = vod_copy(p, metadata_parts[MP4_METADATA_PART_FTYP].data, ftyp_size);
		}
		else
		{
			mp4_clipper_write_tail(&write_context, MP4_CLIPPER_INDEX_FTYP_HEADER, p - ATOM_HEADER_SIZE, ATOM_HEADER_SIZE);
			mp4_clipper_write_tail(&write_context, MP4_CLIPPER_INDEX_FTYP_DATA, metadata_parts[MP4_METADATA_PART_FTYP].data, ftyp_size);
		}
	}

	// moov
	write_atom_header(p, mp4_parse_result->moov_atom_size, 'm', 'o', 'o', 'v');

	// moov.mvhd
	if (mp4_parse_result->copy_data)
	{
		copy_full_atom(p, mp4_parse_result->mvhd.atom);
		mp4_clipper_mvhd_update_atom(p - mp4_parse_result->mvhd.atom.size, &mp4_parse_result->mvhd);
	}
	else
	{
		mp4_clipper_write_tail(&write_context, MP4_CLIPPER_INDEX_MOOV_HEADER, p - ATOM_HEADER_SIZE, ATOM_HEADER_SIZE);
		mp4_clipper_mvhd_update_atom((u_char*)mp4_parse_result->mvhd.atom.ptr, &mp4_parse_result->mvhd);
		write_full_atom(&write_context, MP4_CLIPPER_INDEX_MVHD_ATOM, mp4_parse_result->mvhd.atom);
	}

	// moov.trak
	write_context.elts += MP4_CLIPPER_INDEX_COUNT;
	cur_trak = mp4_parse_result->parsed_traks.elts;
	last_trak = cur_trak + mp4_parse_result->parsed_traks.nelts;
	for (; cur_trak < last_trak; cur_trak++)
	{
		p = mp4_clipper_trak_write_atom(p, &write_context, *cur_trak, mp4_parse_result->copy_data, chunk_pos_diff);
		write_context.elts += MP4_CLIPPER_TRAK_INDEX_COUNT;
	}
	write_context.elts = write_elts;

	// mdat
	if (mdat_header_size == ATOM_HEADER_SIZE)
	{
		write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');
	}
	else
	{
		write_atom_header64(p, mdat_atom_size, 'm', 'd', 'a', 't');
	}

	if (!mp4_parse_result->copy_data)
	{
		mp4_clipper_write_tail(&write_context, MP4_CLIPPER_INDEX_MDAT_HEADER, p - mdat_header_size, mdat_header_size);
	}

	// write the whole moov in case of copy
	if (mp4_parse_result->copy_data)
	{
		mp4_clipper_write_tail(&write_context, 0, output, p - output);
	}

	if (p - output != (ssize_t)mp4_parse_result->alloc_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_clipper_build_header: alloc size %uz different than used size %O", mp4_parse_result->alloc_size, (off_t)(p - output));
		return VOD_UNEXPECTED;
	}

	// null terminate the list
	*write_context.last = NULL;

	// return the response size
	*response_size = ftyp_atom_size + mp4_parse_result->moov_atom_size + mdat_atom_size;
	*content_type = mp4_content_type;

	return VOD_OK;
}
