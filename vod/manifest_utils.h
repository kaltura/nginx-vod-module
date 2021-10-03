#ifndef __MANIFEST_UTILS_H__
#define __MANIFEST_UTILS_H__

// includes
#include "media_set.h"

// constants
#define ADAPTATION_SETS_FLAG_MUXED					(0x1)
#define ADAPTATION_SETS_FLAG_EXCLUDE_MUXED_AUDIO	(0x2)
#define ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK		(0x4)
#define ADAPTATION_SETS_FLAG_AVOID_AUDIO_ONLY		(0x8)
#define ADAPTATION_SETS_FLAG_DEFAULT_LANG_LAST		(0x10)
#define ADAPTATION_SETS_FLAG_MULTI_AUDIO_CODEC		(0x20)
#define ADAPTATION_SETS_FLAG_MULTI_VIDEO_CODEC		(0x40)

#define ADAPTATION_SETS_FLAG_MULTI_CODEC			(ADAPTATION_SETS_FLAG_MULTI_AUDIO_CODEC | ADAPTATION_SETS_FLAG_MULTI_VIDEO_CODEC)

#define MANIFEST_UTILS_TRACKS_SPEC_MAX_SIZE (sizeof("-f-v-f-a") - 1 + VOD_INT32_LEN * 4)

// enums
enum {		// Note: must match media type in order
	ADAPTATION_TYPE_VIDEO,
	ADAPTATION_TYPE_AUDIO,
	ADAPTATION_TYPE_SUBTITLE,
	ADAPTATION_TYPE_MUXED,
	ADAPTATION_TYPE_COUNT,
};

// typedefs
typedef struct {
	media_track_t** first;		// [count * (type == muxed ? MEDIA_TYPE_COUNT : 1)]
	media_track_t** last;
	uint32_t type;
	uint32_t count;
} adaptation_set_t;

typedef struct {
	adaptation_set_t* first;	// [total_count]
	adaptation_set_t* last;
	adaptation_set_t* first_by_type[ADAPTATION_TYPE_COUNT];
	uint32_t count[ADAPTATION_TYPE_COUNT];
	uint32_t total_count;
	bool_t multi_audio;
} adaptation_sets_t;

// functions
vod_status_t manifest_utils_build_request_params_string(
	request_context_t* request_context,
	track_mask_t* has_tracks,
	uint32_t segment_index,
	uint32_t sequences_mask,
	sequence_tracks_mask_t* sequence_tracks_mask,
	sequence_tracks_mask_t* sequence_tracks_mask_end,
	track_mask_t* tracks_mask,
	vod_str_t* result);

u_char* manifest_utils_append_tracks_spec(
	u_char* p,
	media_track_t** tracks,
	uint32_t track_count,
	bool_t write_sequence_index);

vod_status_t manifest_utils_get_adaptation_sets(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	adaptation_sets_t* output);

#endif //__MANIFEST_UTILS_H__
