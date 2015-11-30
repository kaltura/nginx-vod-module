#ifndef __MP4_PARSER_H__
#define __MP4_PARSER_H__

// includes
#include "mp4_parser_base.h"
#include "../input/frames_source.h"
#include "../input/read_cache.h"
#include "../codec_config.h"
#include "../common.h"

// macros
#define rescale_time(time, cur_scale, new_scale) ((((uint64_t)(time)) * (new_scale) + (cur_scale) / 2) / (cur_scale))

// constants
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

// media info
#define PARSE_FLAG_CODEC_NAME			(0x00000001)
#define PARSE_FLAG_EXTRA_DATA			(0x00000002)
#define PARSE_FLAG_EXTRA_DATA_SIZE		(0x00000004)
#define PARSE_FLAG_EXTRA_DATA_PARSE		(0x00000008)
#define PARSE_FLAG_SAVE_RAW_ATOMS		(0x00000010)

// frames
#define PARSE_FLAG_FRAMES_DURATION		(0x00010000)
#define PARSE_FLAG_FRAMES_PTS_DELAY		(0x00020000)
#define PARSE_FLAG_FRAMES_SIZE			(0x00040000)
#define PARSE_FLAG_FRAMES_OFFSET		(0x00080000)
#define PARSE_FLAG_FRAMES_IS_KEY		(0x00100000)
#define PARSE_FLAG_DURATION_LIMITS		(0x00200000)
#define PARSE_FLAG_TOTAL_SIZE_ESTIMATE	(0x00400000)

// media set
#define PARSE_FLAG_ALL_CLIPS			(0x01000000)

// flag groups
#define PARSE_FLAG_FRAMES_ALL (PARSE_FLAG_FRAMES_DURATION | PARSE_FLAG_FRAMES_PTS_DELAY | PARSE_FLAG_FRAMES_SIZE | PARSE_FLAG_FRAMES_OFFSET | PARSE_FLAG_FRAMES_IS_KEY)
#define PARSE_FLAG_FRAMES_ALL_EXCEPT_OFFSETS (PARSE_FLAG_FRAMES_DURATION | PARSE_FLAG_FRAMES_PTS_DELAY	| PARSE_FLAG_FRAMES_SIZE | PARSE_FLAG_FRAMES_IS_KEY)
#define PARSE_FLAG_PARSED_EXTRA_DATA (PARSE_FLAG_EXTRA_DATA_PARSE | PARSE_FLAG_EXTRA_DATA)
#define PARSE_FLAG_PARSED_EXTRA_DATA_SIZE (PARSE_FLAG_EXTRA_DATA_PARSE | PARSE_FLAG_EXTRA_DATA_SIZE)
#define PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE (PARSE_FLAG_DURATION_LIMITS | PARSE_FLAG_TOTAL_SIZE_ESTIMATE)
#define PARSE_BASIC_METADATA_ONLY (0)

// enums
enum {			// raw track atoms
	RTA_TKHD,
	RTA_HDLR,
	RTA_MDHD,
	RTA_DINF,
	RTA_STSD,

	RTA_COUNT
};

// typedefs
struct media_clip_source_s;

typedef struct {
	const u_char* ptr;
	uint64_t size;
	uint8_t header_size;
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
	mp4a_config_t codec_config;
} audio_media_info_t;

typedef struct media_info_s {
	uint32_t media_type;
	uint32_t format;
	uint32_t track_id;
	uint32_t timescale;
	uint32_t frames_timescale;
	uint64_t full_duration;
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
} media_info_t;

typedef struct {
	vod_array_t tracks;		// array of mp4_track_base_metadata_t
	raw_atom_t mvhd_atom;
	uint64_t duration;
	uint32_t timescale;
} mp4_base_metadata_t;

typedef struct {
	struct media_clip_source_s* source;
	vod_str_t uri;
	void* drm_info;
} file_info_t;

struct input_frame_s {
	uint32_t duration;
	uint32_t size;
	uint32_t key_frame;
	uint32_t pts_delay;
};

typedef struct input_frame_s input_frame_t;

typedef struct {
	u_char* auxiliary_info;
	u_char* auxiliary_info_end;
	uint8_t default_auxiliary_sample_size;
	u_char* auxiliary_sample_sizes;		// [frame_count]
	bool_t use_subsamples;
} media_encryption_t;

typedef struct {
	media_info_t media_info;
	file_info_t file_info;
	uint32_t index;
	input_frame_t* first_frame;
	input_frame_t* last_frame;
	frames_source_t* frames_source;
	void* frames_source_context;
	uint64_t* frame_offsets;		// Saved outside input_frame_t since it's not needed for iframes file
	uint32_t frame_count;
	uint32_t key_frame_count;
	uint64_t total_frames_size;
	uint64_t total_frames_duration;
	uint32_t first_frame_index;
	uint64_t first_frame_time_offset;
	uint64_t clip_sequence_offset;
	int32_t clip_from_frame_offset;
	raw_atom_t raw_atoms[RTA_COUNT];
	void* source_clip;
	media_encryption_t encryption_info;
} media_track_t;

typedef struct {
	media_track_t* first_track;
	media_track_t* last_track;
	uint32_t total_track_count;
	uint32_t track_count[MEDIA_TYPE_COUNT];
	raw_atom_t mvhd_atom;
} media_track_array_t;

// functions
vod_status_t mp4_parser_get_ftyp_atom_into(
	request_context_t* request_context,
	const u_char* buffer,
	size_t buffer_size,
	const u_char** ptr,
	size_t* size);

vod_status_t mp4_parser_get_moov_atom_info(
	request_context_t* request_context, 
	const u_char* buffer, 
	size_t buffer_size, 
	off_t* offset, 
	size_t* size);

vod_status_t mp4_parser_uncompress_moov(
	request_context_t* request_context,
	const u_char* buffer,
	size_t size,
	size_t max_moov_size,
	u_char** out_buffer,
	off_t* moov_offset,
	size_t* moov_size);

vod_status_t mp4_parser_parse_basic_metadata(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	const u_char* buffer,
	size_t size,
	file_info_t* file_info,
	mp4_base_metadata_t* result);

vod_status_t mp4_parser_parse_frames(
	request_context_t* request_context,
	mp4_base_metadata_t* base,
	media_parse_params_t* parse_params,
	bool_t align_segments_to_key_frames,
	read_cache_state_t* read_cache_state,
	media_track_array_t* result);

#endif // __MP4_PARSER_H__
