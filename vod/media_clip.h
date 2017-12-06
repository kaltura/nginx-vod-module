#ifndef __MEDIA_CLIP_H__
#define __MEDIA_CLIP_H__

// includes
#include "media_format.h"

// macros
#define media_clip_is_source(type) ((type) < MEDIA_CLIP_SOURCE_LIMIT)

// constants
#define MEDIA_CLIP_KEY_SIZE (16)

// typedefs
struct segmenter_conf_s;
struct audio_filter_s;
struct media_sequence_s;

typedef enum {
	// sources / generators
	MEDIA_CLIP_SOURCE,
	MEDIA_CLIP_SILENCE_GENERATOR,
	MEDIA_CLIP_SOURCE_LIMIT,

	// filters
	MEDIA_CLIP_RATE_FILTER,
	MEDIA_CLIP_MIX_FILTER,
	MEDIA_CLIP_GAIN_FILTER,
	MEDIA_CLIP_CONCAT,
	MEDIA_CLIP_DYNAMIC,
} media_clip_type_t;

typedef struct media_clip_s {
	media_clip_type_t type;
	uint32_t id;
	struct media_clip_s* parent;

	// TODO: the fields below are not required for sources, consider adding another struct
	struct audio_filter_s* audio_filter;
	struct media_clip_s** sources;
	uint32_t source_count;
} media_clip_t;

struct media_clip_source_s;
typedef struct media_clip_source_s media_clip_source_t;

struct media_clip_source_s {
	// base
	media_clip_t base;
	int64_t clip_time;
	media_range_t* range;
	media_track_array_t track_array;
	struct media_sequence_s* sequence;
	uint64_t clip_to;

	// TODO: the fields below are not required for generators, consider adding another struct

	// input params
	vod_str_t uri;				// original uri
	uint64_t clip_from;
	uint32_t tracks_mask[MEDIA_TYPE_COUNT];
	uint32_t time_shift[MEDIA_TYPE_COUNT];
	u_char* encryption_key;

	// derived params
	vod_str_t stripped_uri;		// without any params like clipTo
	vod_str_t mapped_uri;		// in case of mapped mode holds the file path
	u_char file_key[MEDIA_CLIP_KEY_SIZE];

	// runtime members
	void* reader_context;
	media_clip_source_t* next;
	uint64_t last_offset;
};

typedef struct {
	int codec_mask;
	uint32_t track_mask[MEDIA_TYPE_COUNT];
	vod_status_t(*generate)(
		request_context_t* request_context,
		media_parse_params_t* parse_params,
		media_track_array_t* result);
} media_generator_t;

#endif //__MEDIA_CLIP_H__
