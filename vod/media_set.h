#ifndef __MEDIA_SET_H__
#define __MEDIA_SET_H__

// includes
#include "mp4/mp4_aes_ctr.h"
#include "mp4/mp4_parser.h"
#include "media_clip.h"
#include "json_parser.h"

// constants
#define INVALID_SEQUENCE_INDEX (UINT_MAX)
#define INVALID_SEGMENT_INDEX (UINT_MAX)
#define INVALID_SEGMENT_TIME (ULLONG_MAX)
#define INVALID_CLIP_INDEX (UINT_MAX)

#define MAX_CLIPS (128)
#define MAX_CLIPS_PER_REQUEST (16)
#define MAX_SEQUENCES (32)
#define MAX_SOURCES (32)

// typedefs
struct segmenter_conf_s;
struct audio_filter_s;
struct media_sequence_s;
typedef struct media_sequence_s media_sequence_t;

typedef struct {
	uint32_t nom;
	uint32_t denom;
} vod_fraction_t;

typedef struct {
	media_track_t* first_track;
	media_track_t* last_track;
	raw_atom_t mvhd_atom;
	media_track_t* longest_track[MEDIA_TYPE_COUNT];
} media_clip_filtered_t;

struct media_sequence_s {
	// initialized during parsing
	uint32_t index;
	media_clip_t** clips;						// [clip_count]
	vod_str_t stripped_uri;

	// initialized after mapping
	vod_str_t mapped_uri;

	// initialized when the main state machine starts
	u_char uri_key[MEDIA_CLIP_KEY_SIZE];
	u_char encryption_key[MEDIA_CLIP_KEY_SIZE];
	void* drm_info;

	// initialized while applying filters
	uint32_t track_count[MEDIA_TYPE_COUNT];		// track count in each filtered_clips
	uint32_t total_track_count;
	int media_type;
	media_clip_filtered_t* filtered_clips;		// [clip_count]
	media_clip_filtered_t* filtered_clips_end;

	uint64_t total_frame_size;
	uint32_t total_frame_count;
	uint32_t video_key_frame_count;
};

typedef struct {
	// initialized during parsing
	uint32_t total_clip_count;				// number of clips in the whole set
	uint32_t clip_count;					// number of clips relevant to serve the current request
	uint32_t* durations;					// [total_clip_count], in millis
	uint64_t total_duration;
	uint32_t sequence_count;
	media_sequence_t* sequences;			// [sequence_count]
	media_sequence_t* sequences_end;
	bool_t has_multi_sequences;
	media_clip_source_t** sources;
	media_clip_source_t** sources_end;
	bool_t use_discontinuity;
	vod_str_t uri;

	// initialized while applying filters
	uint32_t track_count[MEDIA_TYPE_COUNT];	// sum of track count in all sequences per clip
	uint32_t total_track_count;
	media_track_t* filtered_tracks;			// [total_track_count * clip_count] (all tracks of clip 0, then all tracks of clip1 etc.)
	media_track_t* filtered_tracks_end;
	bool_t audio_filtering_needed;
} media_set_t;

typedef struct {
	uint64_t segment_time;		// used in mss
	uint32_t segment_index;
	uint32_t clip_index;
	uint32_t sequences_mask;
	uint32_t tracks_mask[MEDIA_TYPE_COUNT];
} request_params_t;

#endif //__MEDIA_SET_H__
