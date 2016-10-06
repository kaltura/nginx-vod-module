#ifndef __MEDIA_CLIP_H__
#define __MEDIA_CLIP_H__

// includes
#include "media_format.h"

// constants
#define MEDIA_CLIP_KEY_SIZE (16)

// typedefs
struct segmenter_conf_s;
struct audio_filter_s;
struct media_sequence_s;

typedef enum {
	MEDIA_CLIP_SOURCE,
	MEDIA_CLIP_RATE_FILTER,
	MEDIA_CLIP_MIX_FILTER,
	MEDIA_CLIP_GAIN_FILTER,
	MEDIA_CLIP_CONCAT,
	MEDIA_CLIP_DYNAMIC,
} media_clip_type_t;

typedef struct media_clip_s {
	media_clip_type_t type;
	struct audio_filter_s* audio_filter;
	struct media_clip_s* parent;
	struct media_clip_s** sources;
	uint32_t source_count;
	uint32_t id;
} media_clip_t;

struct media_clip_source_s;
typedef struct media_clip_source_s media_clip_source_t;

struct media_clip_source_s {
	// input params
	media_clip_t base;
	vod_str_t uri;				// original uri
	uint64_t clip_to;
	uint64_t clip_from;
	uint32_t tracks_mask[MEDIA_TYPE_COUNT];
	int64_t clip_time;
	u_char* encryption_key;

	// derived params
	vod_str_t stripped_uri;		// without any params like clipTo
	vod_str_t mapped_uri;		// in case of mapped mode holds the file path
	u_char file_key[MEDIA_CLIP_KEY_SIZE];

	// runtime members
	void* reader_context;
	media_range_t* range;
	media_track_array_t track_array;
	struct media_sequence_s* sequence;
	media_clip_source_t* next;
	uint64_t last_offset;
};

#endif //__MEDIA_CLIP_H__
