#ifndef __MP4_PARSER_H__
#define __MP4_PARSER_H__

// includes
#include "common.h"

// macros
#define rescale_time(time, cur_scale, new_scale) ((((uint64_t)(time)) * (new_scale) + (cur_scale) / 2) / (cur_scale))

// macros for walking the mpeg streams grouped by files
#define WALK_STREAMS_BY_FILES_VARS(cur_file_streams)				\
	mpeg_stream_metadata_t* cur_file_streams[MEDIA_TYPE_COUNT];		\
	mpeg_stream_metadata_t* cur_stream;								\
	file_info_t last_file_info

#define WALK_STREAMS_BY_FILES_START(cur_file_streams, mpeg_metadata)					\
	/* make the last file index different than the file index of the first stream */	\
	last_file_info.file_index = mpeg_metadata->first_stream->file_info.file_index + 1;	\
	vod_memzero(cur_file_streams, sizeof(cur_file_streams));							\
																						\
	for (cur_stream = mpeg_metadata->first_stream;; cur_stream++)						\
	{																					\
		if (cur_stream >= mpeg_metadata->last_stream ||									\
			cur_stream->file_info.file_index != last_file_info.file_index)				\
		{																				\
			if (cur_stream != mpeg_metadata->first_stream)								\
			{

#define WALK_STREAMS_BY_FILES_END(cur_file_streams, mpeg_metadata)						\
			}																			\
																						\
			/* clear the state */														\
			vod_memzero(cur_file_streams, sizeof(cur_file_streams));					\
		}																				\
																						\
		if (cur_stream >= mpeg_metadata->last_stream)									\
		{																				\
			break;																		\
		}																				\
																						\
		cur_file_streams[cur_stream->media_info.media_type] = cur_stream;				\
		last_file_info = cur_stream->file_info;											\
	}

// constants
#define MAX_FRAME_SIZE (10 * 1024 * 1024)
#define MAX_CODEC_NAME_SIZE (64)

// h264 4cc tags
#define FORMAT_AVC1	   (0x31637661)
#define FORMAT_h264	   (0x34363268)
#define FORMAT_H264	   (0x34363248)

// h265 4cc tags
#define FORMAT_HEV1	   (0x31766568)
#define FORMAT_HVC1	   (0x31637668)

// aac 4cc tag
#define FORMAT_MP4A    (0x6134706d)

// parse flags
#define PARSE_TYPE_MASK				(0x0000FFFF)
#define PARSE_FLAG_CODEC_NAME		(0x00010000)
#define PARSE_FLAG_EXTRA_DATA		(0x00020000)
#define PARSE_FLAG_EXTRA_DATA_SIZE	(0x00040000)
#define PARSE_FLAG_EXTRA_DATA_PARSE (0x00080000)
#define PARSE_FLAG_SAVE_RAW_ATOMS	(0x00100000)

enum {
	EXTRA_DATA_SAVE_SIZE = PARSE_FLAG_EXTRA_DATA_SIZE,
	EXTRA_DATA_SAVE_DATA = PARSE_FLAG_EXTRA_DATA,
	EXTRA_DATA_PARSE_SIZE = PARSE_FLAG_EXTRA_DATA_PARSE | PARSE_FLAG_EXTRA_DATA_SIZE,
	EXTRA_DATA_PARSE_DATA = PARSE_FLAG_EXTRA_DATA_PARSE | PARSE_FLAG_EXTRA_DATA,
};

// enums
enum {
	MEDIA_TYPE_VIDEO,
	MEDIA_TYPE_AUDIO,
	MEDIA_TYPE_COUNT,
	MEDIA_TYPE_NONE,
};

enum {
	PARSE_ALL,
	PARSE_ALL_EXCEPT_OFFSETS,
	PARSE_FRAME_DURATIONS_AND_TOTAL_SIZE,
	PARSE_DURATION_LIMITS_AND_TOTAL_SIZE,
	PARSE_TOTAL_SIZE,
	PARSE_BASIC_METADATA_ONLY,
};

enum {			// raw track atoms
	RTA_TKHD,
	RTA_HDLR,
	RTA_MDHD,
	RTA_DINF,
	RTA_STSD,

	RTA_COUNT
};

// typedefs
typedef struct {
	const u_char* ptr;
	uint64_t size;
} raw_atom_t;

typedef struct {
	uint16_t width;
	uint16_t height;
	uint32_t nal_packet_size_length;
} video_media_info_t;

typedef struct {
	uint8_t object_type_id;
	uint16_t channels;
	uint16_t bits_per_sample;
	uint16_t packet_size;
	uint32_t sample_rate;
} audio_media_info_t;

struct media_info_s {
	uint32_t media_type;
	uint32_t format;
	uint32_t track_id;
	uint32_t timescale;
	uint64_t duration;
	uint32_t duration_millis;
	uint32_t bitrate;
	uint32_t min_frame_duration;
	uint32_t max_frame_duration;
	vod_str_t codec_name;
	const u_char* extra_data;
	uint32_t extra_data_size;
	union {
		video_media_info_t video;
		audio_media_info_t audio;
	} u;
};

typedef struct {
	vod_array_t streams;		// array of mpeg_stream_base_metadata_t
	raw_atom_t mvhd_atom;
	uint32_t duration_millis;
	uint64_t duration;
	uint32_t timescale;
	uint32_t max_track_index;
} mpeg_base_metadata_t;

typedef struct {
	uint32_t file_index;
	vod_str_t uri;
} file_info_t;

typedef struct {
	uint32_t duration;
	uint32_t size;
	uint32_t key_frame;
	uint32_t pts_delay;
} input_frame_t;

typedef struct {
	media_info_t media_info;
	file_info_t file_info;
	uint32_t track_index;
	input_frame_t* frames;
	uint64_t* frame_offsets;		// Saved outside input_frame_t since it's not needed for iframes file
	uint32_t frame_count;
	uint32_t key_frame_count;
	uint64_t total_frames_size;
	uint64_t total_frames_duration;
	uint32_t first_frame_time_offset;
	raw_atom_t raw_atoms[RTA_COUNT];
} mpeg_stream_metadata_t;

typedef struct {
	vod_array_t streams;
	mpeg_stream_metadata_t* first_stream;
	mpeg_stream_metadata_t* last_stream;
	uint32_t duration_millis;
	uint64_t duration;
	uint32_t timescale;
	uint32_t video_key_frame_count;
	uint32_t max_track_index;
	raw_atom_t mvhd_atom;
	uint32_t stream_count[MEDIA_TYPE_COUNT];
} mpeg_metadata_t;

// functions
vod_status_t mp4_parser_get_moov_atom_info(
	request_context_t* request_context, 
	const u_char* buffer, 
	size_t buffer_size, 
	off_t* offset, 
	size_t* size);

vod_status_t mp4_parser_init_mpeg_metadata(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata);

vod_status_t mp4_parser_parse_basic_metadata(
	request_context_t* request_context,
	uint32_t* required_tracks_mask,
	uint32_t clip_from,
	uint32_t clip_to,
	const u_char* buffer,
	size_t size,
	file_info_t* file_info,
	mpeg_base_metadata_t* result);

vod_status_t mp4_parser_parse_frames(
	request_context_t* request_context,
	mpeg_base_metadata_t* base,
	mpeg_metadata_t* result);

vod_status_t mp4_parser_finalize_mpeg_metadata(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata);

#endif // __MP4_PARSER_H__
