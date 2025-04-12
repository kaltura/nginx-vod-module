#ifndef __MEDIA_FORMAT_H__
#define __MEDIA_FORMAT_H__

// includes
#include "input/frames_source.h"
#include "input/read_cache.h"
#include "language_code.h"
#include "codec_config.h"

// macros
#define rescale_time(time, cur_scale, new_scale) ((((uint64_t)(time)) * (new_scale) + (cur_scale) / 2) / (cur_scale))
#define rescale_time_neg(time, cur_scale, new_scale) ((time) >= 0 ? rescale_time(time, cur_scale, new_scale) : -rescale_time(-(time), cur_scale, new_scale))

#define vod_track_mask_set_all_bits(mask) vod_set_all_bits((mask), MAX_TRACK_COUNT)
#define vod_track_mask_reset_all_bits(mask) vod_reset_all_bits((mask), MAX_TRACK_COUNT)
#define vod_track_mask_get_number_of_set_bits(mask) vod_get_number_of_set_bits_in_mask((mask), MAX_TRACK_COUNT)
#define vod_track_mask_are_all_bits_set(mask) vod_are_all_bits_set((mask), MAX_TRACK_COUNT)
#define vod_track_mask_is_any_bit_set(mask) vod_is_any_bit_set((mask), MAX_TRACK_COUNT)
#define vod_track_mask_get_lowest_bit_set(mask) vod_get_lowest_bit_set((mask), MAX_TRACK_COUNT)
#define vod_track_mask_and_bits(dst, a, b) vod_and_bits((dst), (a), (b), MAX_TRACK_COUNT)

// constants
#define MAX_CODEC_NAME_SIZE (64)
#define MAX_FRAME_SIZE (10 * 1024 * 1024)
#ifdef NGX_VOD_MAX_TRACK_COUNT
#if NGX_VOD_MAX_TRACK_COUNT % 64 != 0
#error MAX_TRACK_COUNT must be a multiple of 64!
#endif
#define MAX_TRACK_COUNT (NGX_VOD_MAX_TRACK_COUNT)
#define MAX_TRACK_INDEX_LEN (sizeof(ngx_value(NGX_VOD_MAX_TRACK_COUNT)) - 1)
#else
#define MAX_TRACK_COUNT (64)
#define MAX_TRACK_INDEX_LEN (2)
#endif
#define MAX_DURATION_SEC (1000000)
#define MAX_CLIP_DURATION (90000000)		// 25h
#define MAX_SEQUENCE_DURATION (864000000)		// 10 days

// read flags
#define MEDIA_READ_FLAG_REALLOC_BUFFER	(0x1)
#define MEDIA_READ_FLAG_ALLOW_EMPTY_READ (0x2)

// parse flags

// media info
#define PARSE_FLAG_CODEC_NAME			(0x00000001)
#define PARSE_FLAG_EXTRA_DATA			(0x00000002)
#define PARSE_FLAG_EXTRA_DATA_SIZE		(0x00000004)
#define PARSE_FLAG_EXTRA_DATA_PARSE		(0x00000008)
#define PARSE_FLAG_SAVE_RAW_ATOMS		(0x00000010)		// mp4 only
#define PARSE_FLAG_EDIT_LIST			(0x00000020)
#define PARSE_FLAG_HDLR_NAME			(0x00000040)
#define PARSE_FLAG_UDTA_NAME			(0x00000080)
#define PARSE_FLAG_CODEC_TRANSFER_CHAR	(0x00000100)

// frames
#define PARSE_FLAG_FRAMES_DURATION		(0x00010000)
#define PARSE_FLAG_FRAMES_PTS_DELAY		(0x00020000)
#define PARSE_FLAG_FRAMES_SIZE			(0x00040000)
#define PARSE_FLAG_FRAMES_OFFSET		(0x00080000)
#define PARSE_FLAG_FRAMES_IS_KEY		(0x00100000)
#define PARSE_FLAG_DURATION_LIMITS		(0x00200000)
#define PARSE_FLAG_TOTAL_SIZE_ESTIMATE	(0x00400000)
#define PARSE_FLAG_RELATIVE_TIMESTAMPS	(0x00800000)		// relative to segment
#define PARSE_FLAG_INITIAL_PTS_DELAY	(0x01000000)
#define PARSE_FLAG_KEY_FRAME_BITRATE	(0x02000000)

// flag groups
#define PARSE_FLAG_FRAMES_ALL (PARSE_FLAG_FRAMES_DURATION | PARSE_FLAG_FRAMES_PTS_DELAY | PARSE_FLAG_FRAMES_SIZE | PARSE_FLAG_FRAMES_OFFSET | PARSE_FLAG_FRAMES_IS_KEY)
#define PARSE_FLAG_FRAMES_ALL_EXCEPT_OFFSETS (PARSE_FLAG_FRAMES_DURATION | PARSE_FLAG_FRAMES_PTS_DELAY | PARSE_FLAG_FRAMES_SIZE | PARSE_FLAG_FRAMES_IS_KEY)
#define PARSE_FLAG_PARSED_EXTRA_DATA (PARSE_FLAG_EXTRA_DATA_PARSE | PARSE_FLAG_EXTRA_DATA)
#define PARSE_FLAG_PARSED_EXTRA_DATA_SIZE (PARSE_FLAG_EXTRA_DATA_PARSE | PARSE_FLAG_EXTRA_DATA_SIZE)
#define PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE (PARSE_FLAG_DURATION_LIMITS | PARSE_FLAG_TOTAL_SIZE_ESTIMATE)
#define PARSE_BASIC_METADATA_ONLY (0)

// request flags
#define REQUEST_FLAG_SINGLE_TRACK					(0x01)
#define REQUEST_FLAG_SINGLE_TRACK_PER_MEDIA_TYPE	(0x02)
#define REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE			(0x04)
#define REQUEST_FLAG_PARSE_ALL_CLIPS				(0x08)
#define REQUEST_FLAG_LOOK_AHEAD_SEGMENTS			(0x10)
#define REQUEST_FLAG_NO_DISCONTINUITY				(0x20)
#define REQUEST_FLAG_FORCE_PLAYLIST_TYPE_VOD		(0x40)

// audio channels (aligned with ffmpeg AV_CH_XXX)
#define VOD_CH_FRONT_LEFT				0x00000001
#define VOD_CH_FRONT_RIGHT				0x00000002
#define VOD_CH_FRONT_CENTER				0x00000004
#define VOD_CH_LOW_FREQUENCY			0x00000008
#define VOD_CH_BACK_LEFT				0x00000010
#define VOD_CH_BACK_RIGHT				0x00000020
#define VOD_CH_FRONT_LEFT_OF_CENTER		0x00000040
#define VOD_CH_FRONT_RIGHT_OF_CENTER	0x00000080
#define VOD_CH_BACK_CENTER				0x00000100
#define VOD_CH_SIDE_LEFT				0x00000200
#define VOD_CH_SIDE_RIGHT				0x00000400
#define VOD_CH_TOP_CENTER				0x00000800
#define VOD_CH_TOP_FRONT_LEFT			0x00001000
#define VOD_CH_TOP_FRONT_CENTER			0x00002000
#define VOD_CH_TOP_FRONT_RIGHT			0x00004000
#define VOD_CH_TOP_BACK_LEFT			0x00008000
#define VOD_CH_TOP_BACK_CENTER			0x00010000
#define VOD_CH_TOP_BACK_RIGHT			0x00020000
#define VOD_CH_WIDE_LEFT				0x0000000080000000ULL
#define VOD_CH_WIDE_RIGHT				0x0000000100000000ULL
#define VOD_CH_SURROUND_DIRECT_LEFT		0x0000000200000000ULL
#define VOD_CH_SURROUND_DIRECT_RIGHT	0x0000000400000000ULL
#define VOD_CH_LOW_FREQUENCY_2			0x0000000800000000ULL

#define VOD_CH_LAYOUT_MONO				(VOD_CH_FRONT_CENTER)
#define VOD_CH_LAYOUT_STEREO		  	(VOD_CH_FRONT_LEFT|VOD_CH_FRONT_RIGHT)
#define VOD_CH_LAYOUT_SURROUND			(VOD_CH_LAYOUT_STEREO|VOD_CH_FRONT_CENTER)
#define VOD_CH_LAYOUT_2_1			 	(VOD_CH_LAYOUT_STEREO|VOD_CH_BACK_CENTER)
#define VOD_CH_LAYOUT_4POINT0		 	(VOD_CH_LAYOUT_SURROUND|VOD_CH_BACK_CENTER)
#define VOD_CH_LAYOUT_2_2			 	(VOD_CH_LAYOUT_STEREO|VOD_CH_SIDE_LEFT|VOD_CH_SIDE_RIGHT)
#define VOD_CH_LAYOUT_5POINT0		 	(VOD_CH_LAYOUT_SURROUND|VOD_CH_SIDE_LEFT|VOD_CH_SIDE_RIGHT)
#define VOD_CH_LAYOUT_5POINT0_BACK	  	(VOD_CH_LAYOUT_SURROUND|VOD_CH_BACK_LEFT|VOD_CH_BACK_RIGHT)
#define VOD_CH_LAYOUT_5POINT1_BACK	  	(VOD_CH_LAYOUT_5POINT0_BACK|VOD_CH_LOW_FREQUENCY)
#define VOD_CH_LAYOUT_7POINT1_WIDE_BACK	(VOD_CH_LAYOUT_5POINT1_BACK|VOD_CH_FRONT_LEFT_OF_CENTER|VOD_CH_FRONT_RIGHT_OF_CENTER)

#define VOD_CODEC_FLAG(name) (1 << (VOD_CODEC_ID_##name - 1))

#define vod_codec_in_mask(codec_id, mask) (((mask) & (1 << ((codec_id) - 1))) != 0)

// enums
enum {
	VOD_CODEC_ID_INVALID,

	// video
	VOD_CODEC_ID_VIDEO,
	VOD_CODEC_ID_AVC = VOD_CODEC_ID_VIDEO,
	VOD_CODEC_ID_HEVC,
	VOD_CODEC_ID_VP8,
	VOD_CODEC_ID_VP9,
	VOD_CODEC_ID_AV1,

	// audio
	VOD_CODEC_ID_AUDIO,
	VOD_CODEC_ID_AAC = VOD_CODEC_ID_AUDIO,
	VOD_CODEC_ID_AC3,
	VOD_CODEC_ID_EAC3,
	VOD_CODEC_ID_MP3,
	VOD_CODEC_ID_DTS,
	VOD_CODEC_ID_VORBIS,
	VOD_CODEC_ID_OPUS,
	VOD_CODEC_ID_VOLUME_MAP,
	VOD_CODEC_ID_FLAC,

	// captions
	VOD_CODEC_ID_SUBTITLE,
	VOD_CODEC_ID_WEBVTT = VOD_CODEC_ID_SUBTITLE,

	VOD_CODEC_ID_COUNT
};

enum {
	FORMAT_ID_MP4,
	FORMAT_ID_MKV,
	FORMAT_ID_WEBVTT,
	FORMAT_ID_CAP,
	FORMAT_ID_DFXP,
};

enum {			// mp4 only
	RTA_STSD,

	RTA_COUNT
};

// types
typedef uint64_t track_mask_t[vod_array_length_for_bits(MAX_TRACK_COUNT)];

// parse params
typedef struct {
	uint64_t start;			// relative to clip_from
	uint64_t end;			// relative to clip_from
	uint32_t timescale;
	int64_t original_clip_time;
} media_range_t;

typedef struct {
	track_mask_t* required_tracks_mask;
	uint64_t* langs_mask;
	uint32_t clip_from;
	uint32_t clip_to;
	media_range_t* range;
	uint32_t max_frame_count;
	size_t max_frames_size;
	int parse_type;
	int codecs_mask;
	struct media_clip_source_s* source;
} media_parse_params_t;

// typedefs
struct segmenter_conf_s;

typedef struct {
	const u_char* ptr;
	uint64_t size;
	uint8_t header_size;
} raw_atom_t;

typedef struct {
	uint8_t profile;
	uint8_t level;
} dovi_video_media_info_t;

typedef struct {
	uint16_t width;
	uint16_t height;
	uint32_t nal_packet_size_length;
	uint32_t initial_pts_delay;
	uint32_t key_frame_bitrate;
	uint8_t transfer_characteristics;
	dovi_video_media_info_t dovi;
} video_media_info_t;

typedef struct {
	uint8_t object_type_id;
	uint16_t channels;
	uint64_t channel_layout;
	uint16_t bits_per_sample;
	uint16_t packet_size;
	uint32_t sample_rate;
	mp4a_config_t codec_config;
} audio_media_info_t;

typedef struct {
	language_id_t language;
	vod_str_t lang_str;
	vod_str_t label;
	bool_t is_forced;
	vod_str_t characteristics;
	bool_t is_autoselect;
	bool_t is_default;
} media_tags_t;

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
	uint32_t avg_bitrate;
	uint32_t min_frame_duration;	// valid only for video		XXXXX move to video_media_info_t
	uint32_t codec_id;
	vod_str_t codec_name;
	vod_str_t extra_data;
	int64_t empty_duration;		// temporary during parsing
	int64_t start_time;			// temporary during parsing
	uint64_t codec_delay;
	media_tags_t tags;
	union {
		video_media_info_t video;
		audio_media_info_t audio;
	} u;
} media_info_t;

typedef struct {
	struct media_clip_source_s* source;
	vod_str_t uri;
	void* drm_info;
} file_info_t;

struct input_frame_s {
	uint64_t offset;
	uint32_t size;
	uint32_t key_frame;
	uint32_t duration;
	uint32_t pts_delay;
};

typedef struct input_frame_s input_frame_t;

typedef struct frame_list_part_s {
	struct frame_list_part_s* next;
	input_frame_t* first_frame;
	input_frame_t* last_frame;
	uint32_t clip_to;
	frames_source_t* frames_source;
	void* frames_source_context;
} frame_list_part_t;

typedef struct {		// mp4 only
	u_char* auxiliary_info;
	u_char* auxiliary_info_end;
	uint8_t default_auxiliary_sample_size;
	u_char* auxiliary_sample_sizes;		// [frame_count]
	bool_t use_subsamples;
} media_encryption_t;

typedef struct media_track_s {
	media_info_t media_info;
	file_info_t file_info;
	uint32_t index;
	frame_list_part_t frames;
	uint32_t frame_count;
	uint32_t key_frame_count;
	uint64_t total_frames_size;
	uint64_t total_frames_duration;
	uint32_t first_frame_index;
	uint64_t first_frame_time_offset;
	int64_t clip_start_time;
	int64_t original_clip_time;
	int32_t clip_from_frame_offset;
	raw_atom_t raw_atoms[RTA_COUNT];		// mp4 only
	void* source_clip;
	media_encryption_t encryption_info;
	struct media_track_s* next;
} media_track_t;

typedef struct {
	media_track_t* first_track;
	media_track_t* last_track;
	uint32_t total_track_count;
	uint32_t track_count[MEDIA_TYPE_COUNT];
} media_track_array_t;

typedef struct {
	uint64_t read_offset;
	size_t read_size;
	uint32_t flags;
} media_format_read_request_t;

typedef struct {
	// used when returning VOD_AGAIN
	media_format_read_request_t read_req;

	// used when returning VOD_OK
	vod_str_t* parts;
	size_t part_count;
} media_format_read_metadata_result_t;

typedef struct {
	uint64_t first_offset;
	uint64_t last_offset;
} media_clipper_parse_result_t;

typedef struct {
	vod_array_t tracks;
	uint64_t duration;
	uint32_t timescale;
} media_base_metadata_t;

typedef struct {
	// basic info
	uint32_t id;			// FORMAT_ID_xxx
	vod_str_t name;

	// metadata reader
	vod_status_t(*init_metadata_reader)(
		request_context_t* request_context,
		vod_str_t* buffer,
		size_t max_metadata_size,
		void** ctx);

	vod_status_t(*read_metadata)(
		void* ctx,
		uint64_t offset,
		vod_str_t* buffer,
		media_format_read_metadata_result_t* result);

	// clipper
	vod_status_t(*clipper_parse)(
		request_context_t* request_context,
		media_parse_params_t* parse_params,
		vod_str_t* metadata_parts,
		size_t metadata_part_count,
		bool_t copy_data,
		media_clipper_parse_result_t** result);

	vod_status_t(*clipper_build_header)(
		request_context_t* request_context,
		vod_str_t* metadata_parts,
		size_t metadata_part_count,
		media_clipper_parse_result_t* parse_result,
		vod_chain_t** result,
		size_t* response_size,
		vod_str_t* content_type);

	// parser
	vod_status_t(*parse_metadata)(
		request_context_t* request_context,
		media_parse_params_t* parse_params,
		vod_str_t* metadata_parts,
		size_t metadata_part_count,
		media_base_metadata_t** result);

	vod_status_t(*read_frames)(
		request_context_t* request_context,
		media_base_metadata_t* metadata,
		media_parse_params_t* parse_params,			// parse_params are provided only on the first call
		struct segmenter_conf_s* segmenter,
		read_cache_state_t* read_cache_state,
		vod_str_t* frame_data,						// null on the first call
		media_format_read_request_t* read_req,		// VOD_AGAIN
		media_track_array_t* result);				// VOD_OK

} media_format_t;

// functions
vod_status_t media_format_finalize_track(
	request_context_t* request_context,
	int parse_type,
	media_info_t* media_info);

#endif //__MEDIA_FORMAT_H__
