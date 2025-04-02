#include "mkv_format.h"
#include "mkv_defs.h"
#include "ebml.h"
#include "../input/frames_source_memory.h"
#include "../read_stream.h"
#include "../segmenter.h"

// constants
#define BITRATE_ESTIMATE_SEC (5)
#define FRAMES_PER_PART (160)		// about 4K
#define MAX_GOP_FRAMES (600)		// 10 sec GOP in 60 fps
#define MAX_LACES (256)				// the count is stored in one byte

/* when reading the frames, need to read some extra in order to have the headers
	of the first frame of the next cluster -

	field					max size
	MKV_ID_CLUSTER			(4)
	size 					(8)
	MKV_ID_CLUSTERTIMECODE	(1)
	size					(1)
	value					(8)
	MKV_BLOCKGROUP			(1)
	size					(8)
	MKV_BLOCK				(1)
	size					(8)
	track number			(8)
	timecode				(2)
	flags					(1)
*/
#define READ_FRAMES_EXTRA_SIZE (51)

// prototypes
static vod_status_t mkv_parse_seek_entry(ebml_context_t* context, ebml_spec_t* spec, void* dst);
static vod_status_t mkv_simple_block(ebml_context_t* context, ebml_spec_t* spec, void* dst);
static vod_status_t mkv_parse_frame_estimate_bitrate(ebml_context_t* context, ebml_spec_t* spec, void* dst);
static vod_status_t mkv_block_group(ebml_context_t* context, ebml_spec_t* spec, void* dst);
static vod_status_t mkv_reference_block(ebml_context_t* context, ebml_spec_t* spec, void* dst);

// raw parsing structs
typedef struct {
	uint64_t id;
	uint64_t position;
} mkv_seekhead_t;

typedef struct {
	uint64_t timescale;
	double duration;
} mkv_info_t;

typedef struct {
	uint64_t pixel_width;
	uint64_t pixel_height;
} mkv_track_video_t;

typedef struct {
	double sample_rate;
	uint64_t bitdepth;
	uint64_t channels;
} mkv_track_audio_t;

typedef struct {
	uint64_t num;
	uint64_t type;
	vod_str_t codec_id;
	vod_str_t codec_private;
	vod_str_t language;
	vod_str_t name;
	uint64_t default_duration;
	uint64_t codec_delay;
	union {
		mkv_track_video_t video;
		mkv_track_audio_t audio;
	} u;
} mkv_track_t;

typedef struct {
	uint64_t track;
	uint64_t time;
	uint64_t cluster_pos;
	uint64_t relative_pos;
} mkv_index_t;

typedef struct {
	uint64_t timecode;
} mkv_cluster_t;

typedef struct {
	vod_str_t block;
} mkv_block_group_t;

// matroksa specs

// seekhead
static ebml_spec_t mkv_spec_seekhead_entry[] = {
	{ MKV_ID_SEEKID,				EBML_UINT,		offsetof(mkv_seekhead_t, id),				NULL },
	{ MKV_ID_SEEKPOSITION,			EBML_UINT,		offsetof(mkv_seekhead_t, position),			NULL },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_seekhead[] = {
	{ MKV_ID_SEEKENTRY,				EBML_CUSTOM,	0,											mkv_parse_seek_entry },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_segment[] = {
	{ MKV_ID_SEEKHEAD,				EBML_MASTER,	0,											mkv_spec_seekhead },
	{ 0, EBML_NONE, 0, NULL }
};

// info
static ebml_spec_t mkv_spec_info[] = {
	{ MKV_ID_TIMECODESCALE,			EBML_UINT,		offsetof(mkv_info_t, timescale),			NULL },
	{ MKV_ID_DURATION,				EBML_FLOAT,		offsetof(mkv_info_t, duration),				NULL },
	{ 0, EBML_NONE, 0, NULL }
};

// track
static ebml_spec_t mkv_spec_track_video[] = {
	{ MKV_ID_VIDEOPIXELWIDTH,		EBML_UINT,		offsetof(mkv_track_video_t, pixel_width),	NULL },
	{ MKV_ID_VIDEOPIXELHEIGHT,		EBML_UINT,		offsetof(mkv_track_video_t, pixel_height),	NULL },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_track_audio[] = {
	{ MKV_ID_AUDIOSAMPLINGFREQ,		EBML_FLOAT,		offsetof(mkv_track_audio_t, sample_rate),	NULL },
	{ MKV_ID_AUDIOBITDEPTH,			EBML_UINT,		offsetof(mkv_track_audio_t, bitdepth),		NULL },
	{ MKV_ID_AUDIOCHANNELS,			EBML_UINT,		offsetof(mkv_track_audio_t, channels),		NULL },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_track_fields[] = {
	{ MKV_ID_TRACKNUMBER,			EBML_UINT,		offsetof(mkv_track_t, num),					NULL },
	{ MKV_ID_TRACKTYPE,				EBML_UINT,		offsetof(mkv_track_t, type),				NULL },
	{ MKV_ID_TRACKCODECID,			EBML_STRING,	offsetof(mkv_track_t, codec_id),			NULL },
	{ MKV_ID_TRACKCODECPRIVATE,		EBML_BINARY,	offsetof(mkv_track_t, codec_private),		NULL },
	{ MKV_ID_TRACKDEFAULTDURATION,	EBML_UINT,		offsetof(mkv_track_t, default_duration),	NULL },
	{ MKV_ID_TRACKCODECDELAY,		EBML_UINT,		offsetof(mkv_track_t, codec_delay),			NULL },
	{ MKV_ID_TRACKLANGUAGE,			EBML_STRING,	offsetof(mkv_track_t, language),			NULL },
	{ MKV_ID_TRACKNAME,				EBML_STRING,	offsetof(mkv_track_t, name),				NULL },
	{ MKV_ID_TRACKVIDEO,			EBML_MASTER,	offsetof(mkv_track_t, u.video),				mkv_spec_track_video },
	{ MKV_ID_TRACKAUDIO,			EBML_MASTER,	offsetof(mkv_track_t, u.audio),				mkv_spec_track_audio },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_track[] = {
	{ MKV_ID_TRACKENTRY,			EBML_MASTER,	0,											mkv_spec_track_fields },
	{ 0, EBML_NONE, 0, NULL }
};

// index
static ebml_spec_t mkv_spec_index_pos[] = {
	{ MKV_ID_CUETRACK,				EBML_UINT,		offsetof(mkv_index_t, track),				NULL },
	{ MKV_ID_CUECLUSTERPOSITION,	EBML_UINT,		offsetof(mkv_index_t, cluster_pos),			NULL },
	{ MKV_ID_CUERELATIVEPOSITION,	EBML_UINT,		offsetof(mkv_index_t, relative_pos),		NULL },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_index_entry[] = {
	{ MKV_ID_CUETIME,				EBML_UINT,		offsetof(mkv_index_t, time),				NULL },
	{ MKV_ID_CUETRACKPOSITION,		EBML_MASTER,	0,											mkv_spec_index_pos },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_index[] = {
	{ MKV_ID_POINTENTRY,			EBML_MASTER,	0,											mkv_spec_index_entry },
	{ 0, EBML_NONE, 0, NULL }
};

// cluster
static ebml_spec_t mkv_spec_block_group[] = {
	{ MKV_ID_BLOCK,					EBML_BINARY | EBML_TRUNCATE_SIZE,	offsetof(mkv_block_group_t, block), NULL },
	{ MKV_ID_REFERENCEBLOCK,		EBML_CUSTOM,						0,						mkv_reference_block },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_cluster_fields[] = {
	{ MKV_ID_CLUSTERTIMECODE,		EBML_UINT,		offsetof(mkv_cluster_t, timecode),			NULL },
	{ MKV_ID_SIMPLEBLOCK,			EBML_CUSTOM | EBML_TRUNCATE_SIZE,	0,						mkv_simple_block },
	{ MKV_ID_BLOCKGROUP,			EBML_CUSTOM | EBML_TRUNCATE_SIZE,	0,						mkv_block_group },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_cluster[] = {
	{ MKV_ID_CLUSTER,				EBML_MASTER | EBML_TRUNCATE_SIZE,	0,						mkv_spec_cluster_fields },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_bitrate_estimate_block_group[] = {
	{ MKV_ID_BLOCK,					EBML_CUSTOM | EBML_TRUNCATE_SIZE,	0,						mkv_parse_frame_estimate_bitrate },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_bitrate_estimate_cluster_fields[] = {
	{ MKV_ID_CLUSTERTIMECODE,		EBML_UINT,		offsetof(mkv_cluster_t, timecode),			NULL },
	{ MKV_ID_SIMPLEBLOCK,			EBML_CUSTOM | EBML_TRUNCATE_SIZE,	0,						mkv_parse_frame_estimate_bitrate },
	{ MKV_ID_BLOCKGROUP,			EBML_MASTER | EBML_TRUNCATE_SIZE,	0,						mkv_spec_bitrate_estimate_block_group },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t mkv_spec_bitrate_estimate_cluster[] = {
	{ MKV_ID_CLUSTER,				EBML_MASTER | EBML_TRUNCATE_SIZE,	0,						mkv_spec_bitrate_estimate_cluster_fields },
	{ 0, EBML_NONE, 0, NULL }
};

// enums
enum {
	SECTION_INFO,
	SECTION_TRACKS,
	SECTION_CUES,
	SECTION_LAYOUT,		// a virtual section for holding mkv_base_layout_t
	SECTION_COUNT,

	SECTION_FILE_COUNT = SECTION_LAYOUT,
};

// metadata reader states
enum {
	MRS_INITIAL,
	MRS_READ_SECTION_HEADER,
	MRS_READ_SECTION_DATA,
};

// frame reader states
enum {
	FRS_WAIT_START_KEY_FRAME,
	FRS_WAIT_END_KEY_FRAME,
	FRS_DONE,
};

// typedefs
typedef struct {
	uint64_t position_reference;
	uint64_t segment_size;
} mkv_base_layout_t;

typedef struct {
	uint32_t id;
	uint32_t index;
	uint64_t pos;
} mkv_section_pos_t;

typedef struct {
	mkv_base_layout_t base;
	mkv_section_pos_t positions[SECTION_FILE_COUNT];
} mkv_file_layout_t;

typedef struct {
	media_base_metadata_t base;
	mkv_base_layout_t base_layout;
	vod_str_t cues;
	uint64_t start_time;
	uint64_t end_time;
	uint32_t max_frame_count;
	bool_t parse_frames;
	uint64_t read_offset;
} mkv_base_metadata_t;

typedef struct {
	request_context_t* request_context;
	size_t size_limit;
	int state;
	int section;
	vod_str_t sections[SECTION_COUNT];
	mkv_file_layout_t layout;
	mkv_base_metadata_t result;
	uint64_t read_offset;
} mkv_metadata_reader_state_t;

typedef struct {
	input_frame_t* frame;
	frame_list_part_t* part;
	uint32_t laces;
} mkv_laced_frame_t;

typedef struct {
	mkv_laced_frame_t frame;
	uint64_t timecode;
	input_frame_t* unsorted_frame;
	uint64_t unsorted_timecode;
} mkv_frame_timecode_t;

typedef struct {
	uint64_t track_number;
	bool_t done;

	frame_list_part_t frames;
	frame_list_part_t* last_frames_part;
	uint32_t frame_count;
	uint32_t key_frame_count;
	uint64_t total_frames_size;
	uint64_t total_frames_duration;
	uint64_t first_timecode;

	vod_array_t gop_frames;		// array of mkv_frame_timecode_t
	int32_t min_pts_delay;
} mkv_frame_parse_track_context_t;

typedef struct {
	ebml_context_t context;
	int state;
	uint64_t start_time;
	uint64_t end_time;
	uint32_t max_frame_count;
	mkv_frame_parse_track_context_t* first_track;
	mkv_frame_parse_track_context_t* last_track;
} mkv_frame_parse_context_t;

typedef struct {
	ebml_context_t context;
	uint32_t references;
} mkv_block_group_context_t;

typedef struct {
	uint64_t track_number;
	uint64_t min_frame_timecode;
	uint64_t max_frame_timecode;
	uint64_t total_frames_size;
} mkv_estimate_bitrate_track_context_t;

typedef struct {
	ebml_context_t context;
	mkv_estimate_bitrate_track_context_t* first_track;
	mkv_estimate_bitrate_track_context_t* last_track;
} mkv_estimate_bitrate_context_t;

static vod_str_t mkv_supported_doctypes[] = {
	vod_string("matroska"),
	vod_string("webm"),
	vod_null_string
};

// XXXXX avoid using hardcoded extra data - build according to the first frame instead
static u_char mkv_extra_data_ac3[] = {
	0x50, 0x11, 0xe0
};

static u_char mkv_extra_data_eac3[] = {
	0x07, 0x00, 0x20, 0x0f, 0x00, 0x00
};

static u_char mkv_extra_data_opus[] = {
	0x00, 0x02, 0x01, 0x38, 0x00, 0x00, 0xbb, 0x80, 0x00, 0x00, 0x00
};

static bool_t
mkv_is_doctype_supported(vod_str_t* doctype)
{
	vod_str_t* cur_doctype;

	for (cur_doctype = mkv_supported_doctypes; cur_doctype->len; cur_doctype++)
	{
		if (doctype->len == cur_doctype->len &&
			vod_memcmp(doctype->data, cur_doctype->data, doctype->len) == 0)
		{
			return TRUE;
		}
	}

	return FALSE;
}

static vod_status_t
mkv_metadata_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t max_metadata_size,
	void** ctx)
{
	mkv_metadata_reader_state_t* state;
	ebml_context_t context;
	ebml_header_t header;
	vod_status_t rc;

	context.request_context = request_context;
	context.cur_pos = buffer->data;
	context.end_pos = buffer->data + buffer->len;
	context.offset_delta = -(intptr_t)buffer->data;

	rc = ebml_parse_header(&context, &header);
	if (rc != VOD_OK)
	{
		return VOD_NOT_FOUND;
	}

	if (!mkv_is_doctype_supported(&header.doctype))
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_metadata_reader_init: unsupported doctype \"%V\"", &header.doctype);
		return VOD_NOT_FOUND;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_metadata_reader_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(state, sizeof(*state));
	state->request_context = request_context;
	state->size_limit = max_metadata_size;

	*ctx = state;
	return VOD_OK;
}

static vod_status_t
mkv_parse_seek_entry(ebml_context_t* context, ebml_spec_t* spec, void* dst)
{
	mkv_section_pos_t* dest = dst;
	mkv_seekhead_t seekhead;
	vod_status_t rc;
	int index;

	vod_memzero(&seekhead, sizeof(seekhead));
	rc = ebml_parse_master(context, mkv_spec_seekhead_entry, &seekhead);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mkv_parse_seek_entry: ebml_parse_master failed %i", rc);
		return rc;
	}

	index = -1;

	switch (seekhead.id)
	{
	case MKV_ID_INFO:
		index = SECTION_INFO;
		break;

	case MKV_ID_TRACKS:
		index = SECTION_TRACKS;
		break;

	case MKV_ID_CUES:
		index = SECTION_CUES;
		break;
	}

	if (index >= 0)
	{
		dest[index].id = seekhead.id;
		dest[index].index = index;
		dest[index].pos = seekhead.position;
	}

	return VOD_OK;
}

static int
mkv_compare_section_positions(const void* p1, const void* p2)
{
	uint64_t i1 = ((mkv_section_pos_t*)p1)->pos;
	uint64_t i2 = ((mkv_section_pos_t*)p2)->pos;

	if (i1 < i2)
	{
		return -1;
	}
	else if (i1 > i2)
	{
		return 1;
	}

	return 0;
}

static vod_status_t
mkv_get_file_layout(
	request_context_t* request_context, 
	const u_char* buffer, 
	size_t size, 
	mkv_file_layout_t* result)
{
	ebml_header_t header;
	vod_status_t rc;
	ebml_context_t context;
	uint64_t segment_id;
	int i;

	context.request_context = request_context;
	context.cur_pos = buffer;
	context.end_pos = buffer + size;
	context.offset_delta = -(intptr_t)buffer;

	// ebml header
	rc = ebml_parse_header(&context, &header);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// segment
	rc = ebml_read_id(&context, &segment_id);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_get_file_layout: ebml_read_id failed %i", rc);
		return rc;
	}

	if (segment_id != MKV_ID_SEGMENT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_get_file_layout: expected segment element, got 0x%uxL", segment_id);
		return VOD_BAD_DATA;
	}

	rc = ebml_read_num(&context, &result->base.segment_size, 8, 1);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_get_file_layout: ebml_read_num(segment_size) failed %i", rc);
		return rc;
	}

	if (is_unknown_size(result->base.segment_size, rc))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_get_file_layout: segment size is unknown");
		return VOD_BAD_DATA;
	}

	result->base.position_reference = context.cur_pos - buffer;

	// seekhead
	rc = ebml_parse_single(&context, mkv_spec_segment, result->positions);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_get_file_layout: failed to parse seekhead %i", rc);
		return rc;
	}

	for (i = 0; i < SECTION_FILE_COUNT; i++)
	{
		if (result->positions[i].pos == 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"mkv_get_file_layout: missing position for index %d", i);
			return VOD_BAD_DATA;
		}

		result->positions[i].pos += result->base.position_reference;
	}

	// sort according to position to optimize reading
	qsort(
		result->positions, 
		SECTION_FILE_COUNT, 
		sizeof(result->positions[0]), 
		mkv_compare_section_positions);

	return VOD_OK;
}

static vod_status_t
mkv_metadata_reader_read(
	void* ctx,
	uint64_t offset,
	vod_str_t* buffer,
	media_format_read_metadata_result_t* result)
{
	mkv_metadata_reader_state_t* state = ctx;
	mkv_section_pos_t* position;
	ebml_context_t context;
	vod_status_t rc;
	uint64_t data_pos;
	uint64_t size;
	uint64_t id;
	u_char* start_pos;
	int initial_section = state->section;

	// get the file layout
	if (state->state == MRS_INITIAL)
	{
		rc = mkv_get_file_layout(state->request_context, buffer->data, buffer->len, &state->layout);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	result->read_req.flags = 0;

	for (; state->section < SECTION_FILE_COUNT; state->section++)
	{
		position = state->layout.positions + state->section;

		// read the section header
		if (position->pos < offset || position->pos + 16 >= offset + buffer->len)
		{
			if (state->state >= MRS_READ_SECTION_HEADER && state->section == initial_section)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mkv_metadata_reader_read: truncated file (1)");
				return VOD_BAD_DATA;
			}

			state->state = MRS_READ_SECTION_HEADER;
			state->read_offset = position->pos;

			result->read_req.read_offset = position->pos;
			result->read_req.read_size = 0;
			return VOD_AGAIN;
		}

		start_pos = buffer->data + position->pos - offset;

		context.request_context = state->request_context;
		context.cur_pos = start_pos;
		context.end_pos = buffer->data + buffer->len;
		context.offset_delta = state->read_offset - (intptr_t)buffer->data;

		// section id
		rc = ebml_read_id(&context, &id);
		if (rc < 0)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mkv_metadata_reader_read: failed to parse section id");
			return rc;
		}

		if (id != position->id)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mkv_metadata_reader_read: expected id 0x%uxD got 0x%uxL", position->id, id);
			return VOD_BAD_DATA;
		}

		// section size
		rc = ebml_read_num(&context, &size, 8, 1);
		if (rc < 0)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mkv_metadata_reader_read: ebml_read_num(section_size) failed %i", rc);
			return rc;
		}

		if (is_unknown_size(size, rc))
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mkv_metadata_reader_read: section 0x%uxD has unknown size", position->id);
			return VOD_BAD_DATA;
		}

		if (size > state->size_limit)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mkv_metadata_reader_read: section size %uL exceeds the limit %uz", 
				size, state->size_limit);
			return VOD_BAD_DATA;
		}

		// read the whole section
		data_pos = position->pos + context.cur_pos - start_pos;
		if (data_pos + size > offset + buffer->len)
		{
			if (state->state >= MRS_READ_SECTION_DATA && state->section == initial_section)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mkv_metadata_reader_read: truncated file (2)");
				return VOD_BAD_DATA;
			}

			state->state = MRS_READ_SECTION_DATA;
			state->read_offset = position->pos;

			result->read_req.read_offset = position->pos;
			result->read_req.read_size = size;
			return VOD_AGAIN;
		}

		state->sections[position->index].data = buffer->data + data_pos - offset;
		state->sections[position->index].len = size;
		state->size_limit -= size;
		result->read_req.flags = MEDIA_READ_FLAG_REALLOC_BUFFER;
	}

	state->sections[SECTION_LAYOUT].data = (u_char*)&state->layout.base;
	state->sections[SECTION_LAYOUT].len = sizeof(state->layout.base);

	result->parts = state->sections;
	result->part_count = SECTION_COUNT;

	return VOD_OK;
}

static vod_status_t
mkv_metadata_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* metadata_parts,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	mkv_base_metadata_t* metadata;
	const mkv_codec_type_t* cur_codec;
	media_sequence_t* sequence;
	media_track_t* cur_track;
	ebml_context_t context;
	language_id_t lang_id;
	uint32_t timescale;
	mkv_track_t track;
	mkv_info_t info;
	vod_status_t rc;
	uint32_t media_type;
	uint32_t track_indexes[MEDIA_TYPE_COUNT] = { 0, 0 };
	uint32_t track_index;

	// info
	context.request_context = request_context;
	context.cur_pos = metadata_parts[SECTION_INFO].data;
	context.end_pos = context.cur_pos + metadata_parts[SECTION_INFO].len;
	context.offset_delta = -1;

	vod_memzero(&info, sizeof(info));
	rc = ebml_parse_master(&context, mkv_spec_info, &info);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_metadata_parse: ebml_parse_master(info) failed %i", rc);
		return rc;
	}

	if (info.timescale == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_metadata_parse: timescale is zero");
		return VOD_BAD_DATA;
	}

	timescale = NANOS_PER_SEC / info.timescale;
	if (timescale * info.timescale != NANOS_PER_SEC)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_metadata_parse: unsupported - timescale %uL does not divide %uL", 
			info.timescale, (uint64_t)NANOS_PER_SEC);
		return VOD_BAD_DATA;
	}

	if (info.duration == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_metadata_parse: duration is zero");
		return VOD_BAD_DATA;
	}

	if (info.duration > (uint64_t)MAX_DURATION_SEC * timescale)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_metadata_parse: duration %uL too big", info.duration);
		return VOD_BAD_DATA;
	}

	// tracks
	metadata = vod_alloc(request_context->pool, sizeof(*metadata));
	if (metadata == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_metadata_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	if (vod_array_init(&metadata->base.tracks, request_context->pool, 2, sizeof(*cur_track)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_metadata_parse: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	// if raw atoms were requested, parse the extra data so that the raw atoms can be constructed later
	if ((parse_params->parse_type & PARSE_FLAG_SAVE_RAW_ATOMS) != 0)
	{
		parse_params->parse_type |= PARSE_FLAG_EXTRA_DATA;
	}

	context.cur_pos = metadata_parts[SECTION_TRACKS].data;
	context.end_pos = context.cur_pos + metadata_parts[SECTION_TRACKS].len;

	while (context.cur_pos < context.end_pos)
	{
		// parse the ebml
		vod_memzero(&track, sizeof(track));
		rc = ebml_parse_single(&context, mkv_spec_track, &track);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_metadata_parse: ebml_parse_single(track) failed %i", rc);
			return rc;
		}

		// media type
		switch (track.type)
		{
		case MKV_TRACK_TYPE_VIDEO:
			media_type = MEDIA_TYPE_VIDEO;
			break;

		case MKV_TRACK_TYPE_AUDIO:
			media_type = MEDIA_TYPE_AUDIO;
			break;

		default:
			continue;
		}

		// get codec id
		for (cur_codec = mkv_codec_types; cur_codec->mkv_codec_id.len; cur_codec++)
		{
			if (cur_codec->mkv_codec_id.len == track.codec_id.len &&
				vod_memcmp(cur_codec->mkv_codec_id.data, track.codec_id.data, track.codec_id.len) == 0)
			{
				break;
			}
		}

		if (cur_codec->mkv_codec_id.len == 0)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_metadata_parse: unsupported format \"%V\"",
				&track.codec_id);
			continue;
		}

		if (!vod_codec_in_mask(cur_codec->codec_id, parse_params->codecs_mask))
		{
			vod_log_debug2(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_metadata_parse: codec %uD not supported for this request mask 0x%xd",
				cur_codec->codec_id, parse_params->codecs_mask);
			continue;
		}

		// get the language id
		if (track.language.len >= LANG_ISO639_3_LEN)
		{
			lang_id = lang_parse_iso639_3_code(iso639_3_str_to_int(track.language.data));
		}
		else
		{
			lang_id = 0;
		}

		// inherit the sequence language and label
		sequence = parse_params->source->sequence;
		if (sequence->tags.label.len != 0)
		{
			track.name = sequence->tags.label;

			// Note: it is not possible for the sequence to have a language without a label,
			//              since a default label will be assigned according to the language
			if (sequence->tags.lang_str.len != 0)
			{
				track.language = sequence->tags.lang_str;
				lang_id = sequence->tags.language;
			}
		}

		// is this track required ?
		track_index = track_indexes[media_type]++;
		if (!vod_is_bit_set(parse_params->required_tracks_mask[media_type], track_index))
		{
			continue;
		}

		// filter by language
		if (parse_params->langs_mask != NULL &&
			media_type == MEDIA_TYPE_AUDIO &&
			!vod_is_bit_set(parse_params->langs_mask, lang_id))
		{
			continue;
		}

		if (cur_codec->extra_data_required && track.codec_private.len == 0)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"mkv_metadata_parse: no extra data was parsed for track");
			return VOD_BAD_DATA;
		}

		if (metadata->base.tracks.nelts > MAX_TRACK_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"mkv_metadata_parse: track count exceeded the limit of %i", (vod_int_t)MAX_TRACK_COUNT);
			return VOD_BAD_REQUEST;
		}

		cur_track = vod_array_push(&metadata->base.tracks);
		if (cur_track == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_metadata_parse: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}

		vod_memzero(cur_track, sizeof(*cur_track));

		// inherit the sequence tags
		cur_track->media_info.tags = sequence->tags;

		switch (track.type)
		{
		case MKV_TRACK_TYPE_VIDEO:
			cur_track->media_info.min_frame_duration = rescale_time(track.default_duration, NANOS_PER_SEC, timescale);
			if (cur_track->media_info.min_frame_duration == 0)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mkv_metadata_parse: min frame duration is zero");
				return VOD_BAD_DATA;
			}
			cur_track->media_info.u.video.width = track.u.video.pixel_width;
			cur_track->media_info.u.video.height = track.u.video.pixel_height;
			break;

		case MKV_TRACK_TYPE_AUDIO:
			cur_track->media_info.u.audio.bits_per_sample = track.u.audio.bitdepth;
			cur_track->media_info.u.audio.channels = track.u.audio.channels;
			cur_track->media_info.u.audio.sample_rate = (uint32_t)track.u.audio.sample_rate;
			switch (cur_codec->codec_id)
			{
			case VOD_CODEC_ID_AAC:
				cur_track->media_info.u.audio.object_type_id = 0x40;

				rc = codec_config_mp4a_config_parse(
					request_context,
					&track.codec_private,
					&cur_track->media_info);
				if (rc != VOD_OK)
				{
					return rc;
				}
				break;

			case VOD_CODEC_ID_MP3:
				cur_track->media_info.u.audio.object_type_id = cur_track->media_info.u.audio.sample_rate > 24000 ? 0x6B : 0x69;
				break;
			}
			break;
		}

		cur_track->media_info.tags.lang_str = track.language;
		cur_track->media_info.tags.language = lang_id;
		if (track.name.len > 0)
		{
			cur_track->media_info.tags.label = track.name;
		}
		else if (lang_id > 0)
		{
			lang_get_native_name(lang_id, &cur_track->media_info.tags.label);
		}
		else
		{
			cur_track->media_info.tags.label = track.language;
		}
		cur_track->media_info.media_type = media_type;
		cur_track->media_info.codec_id = cur_codec->codec_id;
		cur_track->media_info.format = cur_codec->format;
		cur_track->media_info.track_id = track.num;
		cur_track->media_info.timescale = timescale;
		cur_track->media_info.frames_timescale = timescale;
		cur_track->media_info.codec_delay = track.codec_delay;
		cur_track->media_info.bitrate = sequence->bitrate[media_type];
		cur_track->media_info.avg_bitrate = sequence->avg_bitrate[media_type];

		// Note: setting the duration of all tracks to the file duration, since there is no efficient
		//	way to get the duration of a track
		cur_track->media_info.duration = (uint64_t)info.duration;
		cur_track->media_info.full_duration = cur_track->media_info.duration;
		cur_track->media_info.duration_millis = rescale_time(cur_track->media_info.duration, timescale, 1000);
		cur_track->media_info.extra_data = track.codec_private;

		switch (cur_track->media_info.codec_id)
		{
		case VOD_CODEC_ID_AC3:
			cur_track->media_info.extra_data.data = mkv_extra_data_ac3;
			cur_track->media_info.extra_data.len = sizeof(mkv_extra_data_ac3);
			break;

		case VOD_CODEC_ID_EAC3:
			cur_track->media_info.extra_data.data = mkv_extra_data_eac3;
			cur_track->media_info.extra_data.len = sizeof(mkv_extra_data_eac3);
			break;

		case VOD_CODEC_ID_OPUS:
			cur_track->media_info.extra_data.data = mkv_extra_data_opus;
			cur_track->media_info.extra_data.len = sizeof(mkv_extra_data_opus);
			break;
		}

		cur_track->index = track_index;

		rc = media_format_finalize_track(
			request_context,
			parse_params->parse_type,
			&cur_track->media_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	if (metadata_parts[SECTION_LAYOUT].len != sizeof(metadata->base_layout))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_metadata_parse: invalid layout size %uz", metadata_parts[SECTION_LAYOUT].len);
		return VOD_UNEXPECTED;
	}

	metadata->base.timescale = timescale;
	metadata->base.duration = info.duration;
	metadata->cues = metadata_parts[SECTION_CUES];
	metadata->base_layout = *(mkv_base_layout_t*)metadata_parts[SECTION_LAYOUT].data;
	*result = &metadata->base;
	return VOD_OK;
}

static vod_status_t
mkv_get_read_frames_request(
	request_context_t* request_context,
	mkv_base_metadata_t* metadata,
	uint32_t end_margin,
	media_format_read_request_t* read_req)
{
	ebml_context_t context;
	media_track_t* cur_track;
	vod_uint_t i;
	uint64_t segment_duration;
	uint64_t seen_tracks_mask;
	uint64_t done_tracks_mask;
	uint64_t all_tracks_mask;
	uint64_t cur_track_mask;
	uint64_t initial_time;
	mkv_index_t prev_index;
	mkv_index_t index;
	vod_status_t rc;
	size_t extra_read_size;
	bool_t align_timestamps;
	bool_t done = FALSE;

	read_req->read_offset = ULLONG_MAX;
	read_req->flags = 0;

	initial_time = 0;
	prev_index.cluster_pos = ULLONG_MAX;
	prev_index.time = 0;

	seen_tracks_mask = 0;
	done_tracks_mask = 0;

	context.request_context = request_context;
	context.cur_pos = metadata->cues.data;
	context.end_pos = context.cur_pos + metadata->cues.len;
	context.offset_delta = -1;

	align_timestamps = TRUE;	// XXXX conf param

	// XXXXX optimize this - it may be possible to use the cuetime as the cluster timestamp, and start mid-cluster
	//		another possible optimization is to read in fixed sizes until the segment is complete (may reduce the total read size)

	for (;;)
	{
		vod_memzero(&index, sizeof(index));
		cur_track_mask = 0;

		if (context.cur_pos < context.end_pos)
		{
			rc = ebml_parse_single(&context, mkv_spec_index, &index);
			if (rc != VOD_OK)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"mkv_get_read_frames_request: ebml_parse_single failed %i", rc);
				return rc;
			}

			for (i = 0; i < metadata->base.tracks.nelts; i++)
			{
				cur_track = (media_track_t*)metadata->base.tracks.elts + i;

				if (index.track == cur_track->media_info.track_id)
				{
					cur_track_mask = 1 << i;
					break;
				}
			}

			seen_tracks_mask |= cur_track_mask;

			vod_log_debug4(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_get_read_frames_request: track=%uL, time=%uL, cluster_pos=%uL, relative_pos=%uL",
				index.track, index.time, index.cluster_pos, index.relative_pos);
		}
		else
		{
			index.time = metadata->base.duration;
			index.cluster_pos = metadata->base_layout.segment_size;
			done = TRUE;
		}

		if (read_req->read_offset == ULLONG_MAX)
		{
			if (align_timestamps &&
				metadata->start_time <= index.time)
			{
				metadata->start_time = index.time;
				read_req->read_offset = index.cluster_pos;
				initial_time = index.time;
			}
			else if (metadata->start_time < index.time &&
				prev_index.cluster_pos != ULLONG_MAX)
			{
				read_req->read_offset = prev_index.cluster_pos;
				initial_time = prev_index.time;
			}
		}

		if (done)
		{
			break;
		}

		if (metadata->end_time <= index.time)
		{
			if (align_timestamps)
			{
				metadata->end_time = index.time;
				align_timestamps = FALSE;
			}

			done_tracks_mask |= cur_track_mask;
			if ((seen_tracks_mask & ~done_tracks_mask) == 0)
			{
				break;
			}
		}

		prev_index = index;
	}

	if (read_req->read_offset == ULLONG_MAX)
	{
		// no frames
		return VOD_OK;
	}

	if (index.cluster_pos <= read_req->read_offset)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_get_read_frames_request: end cue pos %uL is less than start cue pos %uL",
			index.cluster_pos, read_req->read_offset);
		return VOD_BAD_DATA;
	}

	read_req->read_size = index.cluster_pos + index.relative_pos - read_req->read_offset;
	read_req->read_offset += metadata->base_layout.position_reference;

	// since mkv does not contain a duration field per frame, we need to read the timestamp of
	// one additional frame per track
	all_tracks_mask = (1 << metadata->base.tracks.nelts) - 1;
	if (all_tracks_mask & ~seen_tracks_mask)
	{
		// some needed tracks were not included in the index - increase the read size by 1 second
		// in order to read another frame for each track
		segment_duration = 1000;
		if (index.time > initial_time + segment_duration)
		{
			segment_duration = index.time - initial_time;
		}
		extra_read_size = read_req->read_size * 1000 / segment_duration;
	}
	else
	{
		// all required tracks were found in the index, increase the read size by a fixed amount
		// in order to contain the headers of the next frame
		extra_read_size = READ_FRAMES_EXTRA_SIZE;
	}

	read_req->read_size += extra_read_size;

	metadata->read_offset = read_req->read_offset;

	vod_log_debug6(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
		"mkv_get_read_frames_request: reading offsets=%uL..%uL (size=%uz, extra=%uz), time=%uL..%uL",
		read_req->read_offset, read_req->read_offset + read_req->read_size,
		read_req->read_size, extra_read_size,
		metadata->start_time, metadata->end_time);

	return VOD_AGAIN;
}

static void 
mkv_sort_gop_frames(vod_array_t* gop_frames)
{
	mkv_frame_timecode_t* frames = gop_frames->elts;
	mkv_frame_timecode_t* frame1;
	mkv_frame_timecode_t* frame2;
	mkv_laced_frame_t temp_frame;
	uint64_t temp_timecode;
	vod_uint_t index1;
	vod_uint_t index2;
	vod_uint_t limit;
	bool_t done;

	// Note: the last entry is not being sorted (it's the next key frame)
	limit = gop_frames->nelts - 2;

	// Note: using bubble sort since the frames are expected to be nearly sorted
	//		specifically, if the stream does not use B-frames it will be completely sorted

	for (index1 = 0; index1 < limit; index1++)
	{
		done = TRUE;
		for (index2 = limit - index1, frame1 = frames;
			index2 > 0; 
			index2--, frame1 = frame2)
		{
			frame2 = frame1 + 1;
			if (frame1->timecode <= frame2->timecode)
			{
				continue;
			}

			temp_frame = frame1->frame;
			frame1->frame = frame2->frame;
			frame2->frame = temp_frame;

			temp_timecode = frame1->timecode;
			frame1->timecode = frame2->timecode;
			frame2->timecode = temp_timecode;

			done = FALSE;
		}

		if (done)
		{
			break;
		}
	}
}

static void
mkv_update_laces_duration(mkv_laced_frame_t* laced_frame, uint32_t duration)
{
	frame_list_part_t* part;
	input_frame_t* frame;
	uint32_t target_duration;
	uint32_t prev_duration;
	uint32_t laces;
	uint32_t i;

	prev_duration = 0;
	laces = laced_frame->laces;

	part = laced_frame->part;
	frame = laced_frame->frame;
	for (i = 0; i < laces; i++)
	{
		if (frame >= part->last_frame)
		{
			if (part->next == NULL)
			{
				// unexpected
				break;
			}

			part = part->next;
			frame = part->first_frame;
		}

		target_duration = duration * (i + 1) / laces;
		frame->duration = target_duration - prev_duration;

		frame++;
		prev_duration = target_duration;
	}
}

static void
mkv_update_frame_timestamps(mkv_frame_parse_track_context_t* context)
{
	mkv_frame_timecode_t* cur_frame;
	mkv_frame_timecode_t* last_frame;
	uint32_t duration;
	int32_t pts_delay;

	// sort the frames
	if (context->gop_frames.nelts > 2)
	{
		mkv_sort_gop_frames(&context->gop_frames);
	}

	cur_frame = context->gop_frames.elts;
	last_frame = cur_frame + (context->gop_frames.nelts - 1);

	if (cur_frame->frame.frame != NULL)
	{
		// this gop is included in the parsed frames, calculate the pts delay and duration
		for (; cur_frame < last_frame; cur_frame++)
		{
			pts_delay = cur_frame->unsorted_timecode - cur_frame->timecode;
			cur_frame->unsorted_frame->pts_delay = pts_delay;
			if (pts_delay < context->min_pts_delay)
			{
				context->min_pts_delay = pts_delay;
			}

			duration = cur_frame[1].timecode - cur_frame[0].timecode;
			mkv_update_laces_duration(&cur_frame->frame, duration);
			context->total_frames_duration += duration;
		}
	}
	else
	{
		// this gop is not included in the parsed frames, only find the min pts delay
		for (; cur_frame < last_frame; cur_frame++)
		{
			pts_delay = cur_frame->unsorted_timecode - cur_frame->timecode;

			if (pts_delay < context->min_pts_delay)
			{
				context->min_pts_delay = pts_delay;
			}
		}
	}

	// reset the gop frames array
	context->gop_frames.nelts = 0;
}

static uint64_t
mkv_estimate_next_frame_timecode(
	request_context_t* request_context,
	mkv_frame_parse_track_context_t* context)
{
	mkv_frame_timecode_t* cur_frame;
	mkv_frame_timecode_t* last_frame;
	uint32_t laces;
	uint64_t max_timecode;
	uint64_t result;

	// get the number of pending laces + max timecode
	laces = 0;
	max_timecode = 0;

	cur_frame = context->gop_frames.elts;
	last_frame = cur_frame + (context->gop_frames.nelts - 1);
	for (; cur_frame < last_frame; cur_frame++)
	{
		laces += cur_frame->frame.laces;
		if (max_timecode < cur_frame->timecode)
		{
			max_timecode = cur_frame->timecode;
		}
	}

	// estimate the next frame timecode using the average lace duration
	result = max_timecode;
	if (context->frame_count > laces)
	{
		result += context->total_frames_duration / (context->frame_count - laces) * laces;
	}

	vod_log_error(VOD_LOG_WARN, request_context->log, 0,
		"mkv_estimate_next_frame_timecode: estimating next frame timecode, "
		"track_number=%uL, result=%uL, max=%uL, laces=%uD",
		context->track_number, result, max_timecode, laces);

	return result;
}

static vod_status_t
mkv_parse_frame_estimate_bitrate(
	ebml_context_t* context,
	ebml_spec_t* spec,
	void* dst)
{
	mkv_estimate_bitrate_track_context_t* track_context;
	mkv_estimate_bitrate_context_t* estimate_context = vod_container_of(context, mkv_estimate_bitrate_context_t, context);
	mkv_cluster_t* cluster = dst;
	uint64_t frame_timecode;
	uint64_t track_number;
	int16_t timecode;
	vod_status_t rc;

	// get the track context
	rc = ebml_read_num(context, &track_number, 8, 1);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mkv_parse_frame_estimate_bitrate: ebml_read_num(track_number) failed %i", rc);
		return rc;
	}

	for (track_context = estimate_context->first_track; ; track_context++)
	{
		if (track_context >= estimate_context->last_track)
		{
			return VOD_OK;		// unneeded track
		}

		if (track_number == track_context->track_number)
		{
			break;
		}
	}

	// get the timecode
	if (context->cur_pos + 3 > context->end_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mkv_parse_frame_estimate_bitrate: block too small");
		return VOD_BAD_DATA;
	}

	read_be16(context->cur_pos, timecode);
	context->cur_pos++;			// flags

	// update the mix/max timecodes
	frame_timecode = cluster->timecode + timecode;

	if (frame_timecode < track_context->min_frame_timecode)
	{
		track_context->min_frame_timecode = frame_timecode;
	}

	if (frame_timecode > track_context->max_frame_timecode)
	{
		track_context->max_frame_timecode = frame_timecode;
	}

	// update the total size
	track_context->total_frames_size += context->end_pos - context->cur_pos;

	return VOD_OK;
}

static vod_status_t
mkv_parse_frames_estimate_bitrate(
	request_context_t* request_context,
	media_base_metadata_t* base,
	vod_str_t* frame_data,
	media_track_array_t* result)
{
	mkv_estimate_bitrate_track_context_t* track_context;
	mkv_estimate_bitrate_context_t context;
	mkv_base_metadata_t* metadata = vod_container_of(base, mkv_base_metadata_t, base);
	media_track_t* cur_track;
	mkv_cluster_t cluster;
	vod_uint_t i;
	size_t alloc_size = sizeof(context.first_track[0]) * base->tracks.nelts;

	context.first_track = vod_alloc(request_context->pool, alloc_size);
	if (context.first_track == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_parse_frames_estimate_bitrate: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	context.last_track = (void*)((u_char*)context.first_track + alloc_size);

	context.context.request_context = request_context;
	context.context.cur_pos = frame_data->data;
	context.context.end_pos = frame_data->data + frame_data->len;
	context.context.offset_delta = metadata->read_offset - (intptr_t)frame_data->data;

	for (i = 0; i < base->tracks.nelts; i++)
	{
		cur_track = (media_track_t*)base->tracks.elts + i;
		track_context = context.first_track + i;

		track_context->track_number = cur_track->media_info.track_id;
		track_context->min_frame_timecode = ULLONG_MAX;
		track_context->max_frame_timecode = 0;
		track_context->total_frames_size = 0;
	}

	ebml_parse_master(&context.context, mkv_spec_bitrate_estimate_cluster, &cluster);		// ignoring errors

	for (i = 0; i < base->tracks.nelts; i++)
	{
		cur_track = (media_track_t*)base->tracks.elts + i;
		track_context = context.first_track + i;

		if (cur_track->media_info.bitrate == 0 &&
			track_context->max_frame_timecode > track_context->min_frame_timecode)
		{
			cur_track->media_info.bitrate = track_context->total_frames_size * base->timescale * 8 / 
				(track_context->max_frame_timecode - track_context->min_frame_timecode);
		}

		result->track_count[cur_track->media_info.media_type]++;
	}

	return VOD_OK;
}

static vod_status_t
mkv_parse_laces(ebml_context_t* context, uint8_t flags, uint32_t* lace_sizes)
{
	vod_status_t rc;
	uint64_t num;
	int64_t delta;
	size_t total;
	uint32_t laces;
	uint32_t size;
	uint32_t i;
	uint8_t lace_type;
	u_char cur;

	lace_type = (flags & 0x06) >> 1;
	if (lace_type == 0)
	{
		// no lacing
		lace_sizes[0] = context->end_pos - context->cur_pos;
		return 1;
	}

	// get number of laces
	if (context->cur_pos >= context->end_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mkv_parse_laces: overflow while reading number of laces");
		return VOD_BAD_DATA;
	}

	laces = *context->cur_pos + 1;
	context->cur_pos++;

	total = 0;

	switch (lace_type)
	{
	case 0x1: // xiph
		vod_memzero(lace_sizes, (laces - 1) * sizeof(lace_sizes[0]));

		for (i = 0; i < laces - 1; i++)
		{
			do
			{
				if (context->cur_pos >= context->end_pos)
				{
					vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
						"mkv_parse_laces: overflow while reading xiph lace size");
					return VOD_BAD_DATA;
				}

				cur = *context->cur_pos;
				context->cur_pos++;

				lace_sizes[i] += cur;
			} while (cur == 0xff);

			total += lace_sizes[i];
		}
		break;

	case 0x2: // fixed size
		size = context->end_pos - context->cur_pos;
		if (size % laces != 0)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mkv_parse_laces: invalid fixed size lace, size=%uD, laces=%uD", size, laces);
			return VOD_BAD_DATA;
		}

		size /= laces;
		for (i = 0; i < laces; i++)
		{
			lace_sizes[i] = size;
		}
		return laces;

	case 0x3: // EBML
		rc = ebml_read_num(context, &num, 4, 1);
		if (rc < 0)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"mkv_parse_laces: ebml_read_num(initial lace size) failed %i", rc);
			return rc;
		}

		if (num > VOD_MAX_UINT32_VALUE)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mkv_parse_laces: invalid ebml lace size %uL", num);
			return VOD_BAD_DATA;
		}

		lace_sizes[0] = num;
		total = num;

		for (i = 1; i < laces - 1; i++)
		{
			rc = ebml_read_num(context, &num, 4, 1);
			if (rc < 0)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mkv_parse_laces: ebml_read_num(lace size delta) failed %i", rc);
				return rc;
			}

			delta = num - ((1LL << (7 * rc - 1)) - 1);
			if (delta > (int64_t)(VOD_MAX_UINT32_VALUE - lace_sizes[i - 1]))
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mkv_parse_laces: invalid ebml lace delta %L too big", delta);
				return VOD_BAD_DATA;
			}

			if (delta < -(int64_t)lace_sizes[i - 1])
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"mkv_parse_laces: invalid ebml lace delta %L too small", delta);
				return VOD_BAD_DATA;
			}

			lace_sizes[i] = lace_sizes[i - 1] + delta;
			total += lace_sizes[i];
		}
		break;
	}

	size = context->end_pos - context->cur_pos;
	if (size < total)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mkv_parse_laces: laces total size %uz overflows size left %uD", total, size);
		return VOD_BAD_DATA;
	}

	lace_sizes[laces - 1] = size - total;

	return laces;
}

static vod_status_t
mkv_parse_frame(
	mkv_frame_parse_context_t* frame_parse_context,
	ebml_context_t* context, 
	mkv_cluster_t* cluster,
	int key_frame)
{
	mkv_frame_parse_track_context_t* track_context;
	frame_list_part_t* last_frames_part;
	frame_list_part_t* new_frames_part;
	mkv_frame_timecode_t* gop_frame;
	input_frame_t* cur_frame;
	uint64_t frame_timecode;
	uint64_t track_number;
	uint32_t lace_sizes[256];
	intptr_t laces, i;
	int16_t timecode;
	vod_status_t rc;
	uint8_t flags;

	// get the track context
	rc = ebml_read_num(context, &track_number, 8, 1);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mkv_parse_frame: ebml_read_num(track_number) failed %i", rc);
		return rc;
	}

	for (track_context = frame_parse_context->first_track; ; track_context++)
	{
		if (track_context >= frame_parse_context->last_track)
		{
			return VOD_OK;		// unneeded track
		}

		if (track_number == track_context->track_number)
		{
			break;
		}
	}

	if (track_context->done)
	{
		return VOD_OK;
	}

	// get the timecode and flags
	if (context->cur_pos + 3 > context->end_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mkv_parse_frame: block too small");
		return VOD_BAD_DATA;
	}

	read_be16(context->cur_pos, timecode);
	flags = *context->cur_pos++;

	// add a record to the gop frames
	if (track_context->gop_frames.nelts > MAX_GOP_FRAMES)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mkv_parse_frame: gop size exceeds the limit");
		return VOD_BAD_DATA;
	}

	frame_timecode = cluster->timecode + timecode;

	gop_frame = vod_array_push(&track_context->gop_frames);
	if (gop_frame == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mkv_parse_frame: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}
	gop_frame->timecode = frame_timecode;
	gop_frame->unsorted_timecode = frame_timecode;
	gop_frame->frame.frame = NULL;
	gop_frame->unsorted_frame = NULL;

	if (key_frame == -1)
	{
		key_frame = (flags & 0x80) ? 1 : 0;
	}

	if (!key_frame)
	{
		// XXXXX should not cross the clip offset

		if (frame_parse_context->state == FRS_WAIT_START_KEY_FRAME)
		{
			return VOD_OK;
		}
	}
	else
	{
		mkv_update_frame_timestamps(track_context);

		// repush the gop frame following the reset of the array
		gop_frame = vod_array_push(&track_context->gop_frames);		// cant fail

		gop_frame->timecode = frame_timecode;
		gop_frame->unsorted_timecode = frame_timecode;
		gop_frame->frame.frame = NULL;
		gop_frame->unsorted_frame = NULL;

		switch (frame_parse_context->state)
		{
		case FRS_WAIT_START_KEY_FRAME:
			if (frame_timecode < frame_parse_context->start_time || 
				track_context != frame_parse_context->first_track)		// wait for keyframe on the first track only (will be video in case of muxed stream)
			{
				return VOD_OK;
			}

			frame_parse_context->state = FRS_WAIT_END_KEY_FRAME;
			break;

		case FRS_WAIT_END_KEY_FRAME:
			if (frame_timecode < frame_parse_context->end_time ||
				track_context != frame_parse_context->first_track)
			{
				break;
			}

			frame_parse_context->state = FRS_DONE;
			// fall through

		case FRS_DONE:
			track_context->done = TRUE;

			vod_log_debug3(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"mkv_parse_frame: track=%uL, timecode=%uL, key=1, pos=%uL",
				track_number, frame_timecode, (uint64_t)(uintptr_t)context->cur_pos + context->offset_delta);

			// check whether all tracks are done
			for (track_context = frame_parse_context->first_track;
				track_context < frame_parse_context->last_track;
				track_context++)
			{
				if (!track_context->done)
				{
					return VOD_OK;
				}
			}

			return VOD_DONE;
		}
	}

	rc = mkv_parse_laces(context, flags, lace_sizes);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mkv_parse_frame: failed to parse lace sizes %i", rc);
		return rc;
	}

	laces = rc;
	for (i = 0; i < laces; i++)
	{
		// enforce frame count limit
		if (track_context->frame_count >= frame_parse_context->max_frame_count)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"mkv_parse_frame: frame count exceeds the limit %uD", frame_parse_context->max_frame_count);
			return VOD_BAD_DATA;
		}

		last_frames_part = track_context->last_frames_part;

		if (last_frames_part->last_frame >= last_frames_part->first_frame + FRAMES_PER_PART)
		{
			// allocate a new part
			new_frames_part = vod_alloc(context->request_context->pool,
				sizeof(*new_frames_part) + FRAMES_PER_PART * sizeof(input_frame_t));
			if (new_frames_part == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
					"mkv_parse_frame: vod_alloc failed");
				return VOD_ALLOC_FAILED;
			}

			new_frames_part->first_frame = (void*)(new_frames_part + 1);
			new_frames_part->last_frame = new_frames_part->first_frame;
			new_frames_part->frames_source = last_frames_part->frames_source;
			new_frames_part->frames_source_context = last_frames_part->frames_source_context;
			new_frames_part->clip_to = UINT_MAX;		// XXXXX fix this

			last_frames_part->next = new_frames_part;
			track_context->last_frames_part = new_frames_part;
			last_frames_part = new_frames_part;
		}

		// initialize the new frame (duration & pts delay are initialized later)
		cur_frame = last_frames_part->last_frame++;
		cur_frame->key_frame = key_frame;
		cur_frame->duration = 0;
		cur_frame->pts_delay = 0;

		cur_frame->offset = (uintptr_t)context->cur_pos;
		cur_frame->size = lace_sizes[i];
		context->cur_pos += cur_frame->size;

		if (i == 0)
		{
			// add the frame to the gop frames
			gop_frame->frame.frame = cur_frame;
			gop_frame->frame.part = last_frames_part;
			gop_frame->frame.laces = laces;

			gop_frame->unsorted_frame = cur_frame;

			// update the track context
			if (track_context->frame_count == 0)
			{
				track_context->first_timecode = frame_timecode;
			}
		}

		track_context->frame_count++;
		track_context->key_frame_count += key_frame;
		track_context->total_frames_size += cur_frame->size;

		if (laces > 1)
		{
			vod_log_debug7(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"mkv_parse_frame: track=%uL, timecode=%uL, key=%d, pos=%uL, size=%uD, lace=%i/%i",
				track_number, frame_timecode, key_frame, cur_frame->offset + context->offset_delta, cur_frame->size, i + 1, laces);
		}
		else
		{
			vod_log_debug5(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"mkv_parse_frame: track=%uL, timecode=%uL, key=%d, pos=%uL, size=%uD",
				track_number, frame_timecode, key_frame, cur_frame->offset + context->offset_delta, cur_frame->size);
		}
	}

	return VOD_OK;
}

static vod_status_t
mkv_block_group(
	ebml_context_t* context,
	ebml_spec_t* spec,
	void* dst)
{
	mkv_frame_parse_context_t* frame_parse_context = vod_container_of(context, mkv_frame_parse_context_t, context);
	mkv_block_group_context_t block_group_context;
	mkv_block_group_t block_group;
	ebml_context_t block_context;
	mkv_cluster_t* cluster = dst;
	vod_status_t rc;

	block_group_context.context = *context;
	block_group_context.references = 0;

	vod_memzero(&block_group, sizeof(block_group));

	rc = ebml_parse_master(&block_group_context.context, mkv_spec_block_group, &block_group);
	if (rc != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"mkv_block_group: ebml_parse_master(block group) failed");
		return rc;
	}

	if (block_group.block.len <= 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"mkv_block_group: block group without block element");
		return VOD_BAD_DATA;
	}

	block_context.request_context = context->request_context;
	block_context.cur_pos = block_group.block.data;
	block_context.end_pos = block_group.block.data + block_group.block.len;
	block_context.offset_delta = context->offset_delta;

	return mkv_parse_frame(frame_parse_context, &block_context, cluster, block_group_context.references == 0);

}

static vod_status_t
mkv_reference_block(
	ebml_context_t* context,
	ebml_spec_t* spec,
	void* dst)
{
	mkv_block_group_context_t* block_group_context = vod_container_of(context, mkv_block_group_context_t, context);

	block_group_context->references++;

	return VOD_OK;
}

static vod_status_t
mkv_simple_block(
	ebml_context_t* context,
	ebml_spec_t* spec,
	void* dst)
{
	mkv_frame_parse_context_t* frame_parse_context = vod_container_of(context, mkv_frame_parse_context_t, context);
	mkv_cluster_t* cluster = dst;

	return mkv_parse_frame(frame_parse_context, context, cluster, -1);
}

static vod_status_t
mkv_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	vod_str_t* frame_data,
	media_track_array_t* result)
{
	mkv_frame_parse_track_context_t* track_context;
	mkv_frame_parse_context_t frame_parse_context;
	mkv_frame_timecode_t* gop_frame;
	mkv_base_metadata_t* metadata = vod_container_of(base, mkv_base_metadata_t, base);
	frame_list_part_t* part;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	media_track_t* cur_track;
	mkv_cluster_t cluster;
	vod_status_t rc;
	vod_uint_t i;
	size_t alloc_size = sizeof(frame_parse_context.first_track[0]) * base->tracks.nelts;

	// XXXXX support clipping (also set clip_from_time_offset)

	frame_parse_context.first_track = vod_alloc(request_context->pool, alloc_size);
	if (frame_parse_context.first_track == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_parse_frames: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(frame_parse_context.first_track, alloc_size);
	frame_parse_context.last_track = (void*)((u_char*)frame_parse_context.first_track + alloc_size);

	frame_parse_context.context.request_context = request_context;
	frame_parse_context.context.cur_pos = frame_data->data;
	frame_parse_context.context.end_pos = frame_data->data + frame_data->len;
	frame_parse_context.context.offset_delta = metadata->read_offset - (intptr_t)frame_data->data;
	frame_parse_context.start_time = metadata->start_time;
	frame_parse_context.end_time = metadata->end_time;
	frame_parse_context.max_frame_count = metadata->max_frame_count;
	frame_parse_context.state = FRS_WAIT_START_KEY_FRAME;

	for (i = 0; i < base->tracks.nelts; i++)
	{
		cur_track = (media_track_t*)base->tracks.elts + i;
		track_context = frame_parse_context.first_track + i;

		track_context->track_number = cur_track->media_info.track_id;

		// initialize the frames part
		track_context->last_frames_part = &track_context->frames;

		rc = frames_source_memory_init(request_context, &track_context->frames.frames_source_context);
		if (rc != VOD_OK)
		{
			return rc;
		}

		track_context->frames.frames_source = &frames_source_memory;

		track_context->frames.first_frame = vod_alloc(
			request_context->pool, FRAMES_PER_PART * sizeof(input_frame_t));
		if (track_context->frames.first_frame == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_parse_frames: vod_alloc failed (2)");
			return VOD_ALLOC_FAILED;
		}

		// initialize the gop frames array
		if (vod_array_init(&track_context->gop_frames, request_context->pool, 60, sizeof(mkv_frame_timecode_t)) != VOD_OK)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mkv_parse_frames: vod_array_init failed");
			return VOD_ALLOC_FAILED;
		}

		track_context->frames.last_frame = track_context->frames.first_frame;
		track_context->frames.clip_to = UINT_MAX;		// XXXXX fix this
	}

	rc = ebml_parse_master(&frame_parse_context.context, mkv_spec_cluster, &cluster);
	if (rc != VOD_OK && rc != VOD_DONE)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mkv_parse_frames: ebml_parse_master(clusters) failed");
		return rc;
	}

	for (i = 0; i < base->tracks.nelts; i++)
	{
		cur_track = (media_track_t*)base->tracks.elts + i;
		track_context = frame_parse_context.first_track + i;

		track_context->last_frames_part->next = NULL;

		if (track_context->frame_count > 0)
		{
			if (!track_context->done)
			{
				// add a dummy gop entry with the total duration
				gop_frame = vod_array_push(&track_context->gop_frames);
				if (gop_frame == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
						"mkv_parse_frames: vod_array_push failed");
					return VOD_ALLOC_FAILED;
				}
				gop_frame->timecode = mkv_estimate_next_frame_timecode(request_context, track_context);
				gop_frame->unsorted_timecode = gop_frame->timecode;
				gop_frame->frame.frame = NULL;
				gop_frame->unsorted_frame = NULL;

				// close the last gop
				mkv_update_frame_timestamps(track_context);
			}

			if (track_context->min_pts_delay != 0)
			{
				part = &track_context->frames;
				last_frame = part->last_frame;

				// make sure the pts delay is always positive
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

					cur_frame->pts_delay -= track_context->min_pts_delay;
				}
			}
		}

		cur_track->frames = track_context->frames;
		cur_track->frame_count = track_context->frame_count;
		cur_track->first_frame_time_offset = track_context->first_timecode;
		cur_track->key_frame_count = track_context->key_frame_count;
		cur_track->total_frames_size = track_context->total_frames_size;
		cur_track->total_frames_duration = track_context->total_frames_duration;

		// Note: no efficient way to determine first_frame_index

		result->track_count[cur_track->media_info.media_type]++;
	}

	return VOD_OK;
}

static vod_status_t
mkv_prepare_read_frames_request(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	segmenter_conf_t* segmenter,
	media_format_read_request_t* read_req)
{
	mkv_base_metadata_t* metadata = vod_container_of(base, mkv_base_metadata_t, base);
	media_track_t* first_track;
	media_track_t* last_track;
	media_track_t* cur_track;
	uint32_t end_margin;
	uint32_t range;
	bool_t need_bitrate_estimation;
	vod_status_t rc;

	if ((parse_params->parse_type & (PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_TOTAL_SIZE_ESTIMATE)) ==
		PARSE_FLAG_TOTAL_SIZE_ESTIMATE)
	{
		// check whether there are any tracks without bitrate
		first_track = (media_track_t*)base->tracks.elts;
		last_track = first_track + base->tracks.nelts;

		need_bitrate_estimation = FALSE;
		for (cur_track = first_track; cur_track < last_track; cur_track++)
		{
			if (cur_track->media_info.bitrate == 0)
			{
				need_bitrate_estimation = TRUE;
				break;
			}
		}

		if (!need_bitrate_estimation)
		{
			return VOD_OK;
		}
	}

	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_ALL) != 0)
	{
		// Note: must save all the data we'll need from parse params (won't be available in parse frames)
		metadata->start_time = rescale_time(parse_params->range->start, 1000, metadata->base.timescale);
		metadata->end_time = rescale_time(parse_params->range->end, 1000, metadata->base.timescale);
		metadata->max_frame_count = parse_params->max_frame_count;
		metadata->parse_frames = TRUE;
		end_margin = 1000;
	}
	else
	{
		range = BITRATE_ESTIMATE_SEC * metadata->base.timescale;
		if (metadata->base.duration > range)
		{
			metadata->start_time = (metadata->base.duration - range) / 2;
		}
		else
		{
			metadata->start_time = 0;
		}
		metadata->end_time = metadata->start_time + range;
		metadata->parse_frames = FALSE;
		end_margin = 0;
	}

	rc = mkv_get_read_frames_request(
		request_context,
		metadata,
		end_margin,
		read_req);
	if (rc == VOD_OK)
	{
		return VOD_OK;
	}

	if (rc == VOD_AGAIN && read_req->read_size > parse_params->max_frames_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mkv_prepare_read_frames_request: read size %uz exceeds the limit %uz",
			read_req->read_size, parse_params->max_frames_size);
		return VOD_BAD_REQUEST;
	}

	return rc;
}

static vod_status_t
mkv_read_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	segmenter_conf_t* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	mkv_base_metadata_t* metadata = vod_container_of(base, mkv_base_metadata_t, base);
	media_track_t* cur_track;
	vod_status_t rc;

	// TODO: handle initial pts delay

	if (frame_data == NULL &&
		(parse_params->parse_type & (PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_TOTAL_SIZE_ESTIMATE)) != 0)
	{
		rc = mkv_prepare_read_frames_request(
			request_context,
			base,
			parse_params,
			segmenter,
			read_req);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	vod_memzero(result, sizeof(*result));
	result->first_track = (media_track_t*)base->tracks.elts;
	result->last_track = result->first_track + base->tracks.nelts;
	result->total_track_count = base->tracks.nelts;

	if (frame_data == NULL)
	{
		// no need to parse any frames
		for (cur_track = result->first_track; cur_track < result->last_track; cur_track++)
		{
			result->track_count[cur_track->media_info.media_type]++;
		}
		return VOD_OK;
	}

	if (metadata->parse_frames)
	{
		return mkv_parse_frames(request_context, base, frame_data, result);
	}

	return mkv_parse_frames_estimate_bitrate(request_context, base, frame_data, result);
}

media_format_t mkv_format = {
	FORMAT_ID_MKV,
	vod_string("mkv"),
	mkv_metadata_reader_init,
	mkv_metadata_reader_read,
	NULL,
	NULL,
	mkv_metadata_parse,
	mkv_read_frames,
};
