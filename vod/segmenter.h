#ifndef __SEGMENTER_H__
#define __SEGMENTER_H__

// includes
#include "media_set.h"
#include "common.h"

// constants
#define INVALID_SEGMENT_COUNT UINT_MAX
#define SEGMENT_FROM_TIMESTAMP_MARGIN (100)		// in case of clipping, a segment may start up to 2 frames before the segment boundary
#define MIN_SEGMENT_DURATION (500)
#define MAX_SEGMENT_DURATION (600000)

// enums
enum {
	MDP_MAX,
	MDP_MIN,
};

// typedefs
struct segmenter_conf_s;
typedef struct segmenter_conf_s segmenter_conf_t;

typedef struct {
	uint32_t segment_index;
	uint32_t repeat_count;
	uint64_t time;
	uint64_t duration;
	bool_t discontinuity;
} segment_duration_item_t;

typedef struct {
	segment_duration_item_t* items;
	uint32_t item_count;
	uint32_t segment_count;
	uint32_t timescale;
	uint32_t discontinuities;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t duration;
} segment_durations_t;

typedef struct {
	request_context_t* request_context;
	segmenter_conf_t* conf;
	media_clip_timing_t timing;
	uint32_t segment_index;
	int64_t first_key_frame_offset;
	vod_array_part_t* key_frame_durations;
	bool_t allow_last_segment;

	// no discontinuity
	uint64_t last_segment_end;

	// discontinuity
	uint32_t initial_segment_index;

	// gop
	uint64_t time;
} get_clip_ranges_params_t;

typedef struct {
	uint32_t min_clip_index;
	uint32_t max_clip_index;
	uint64_t clip_time;
	media_range_t* clip_ranges;
	uint32_t clip_count;
	uint32_t clip_relative_segment_index;
} get_clip_ranges_result_t;

typedef uint32_t (*segmenter_get_segment_count_t)(segmenter_conf_t* conf, uint64_t duration_millis);

typedef vod_status_t (*segmenter_get_segment_durations_t)(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	uint32_t media_type,
	segment_durations_t* result);

struct segmenter_conf_s {
	// config fields
	uintptr_t segment_duration;
	vod_array_t* bootstrap_segments;		// array of vod_str_t
	bool_t align_to_key_frames;
	intptr_t live_window_duration;
	segmenter_get_segment_count_t get_segment_count;			// last short / last long / last rounded
	segmenter_get_segment_durations_t get_segment_durations;	// estimate / accurate
	vod_uint_t manifest_duration_policy;
	uintptr_t gop_look_behind;
	uintptr_t gop_look_ahead;

	// derived fields
	uint32_t parse_type;
	uint32_t bootstrap_segments_count;
	uint32_t* bootstrap_segments_durations;
	uint32_t max_segment_duration;
	uint32_t max_bootstrap_segment_duration;
	uint32_t bootstrap_segments_total_duration;
	uint32_t* bootstrap_segments_start;
	uint32_t* bootstrap_segments_mid;
	uint32_t* bootstrap_segments_end;
};

typedef struct {
	request_context_t* request_context;
	vod_array_part_t* part;
	int64_t* cur_pos;
	int64_t offset;
} align_to_key_frames_context_t;

// init
vod_status_t segmenter_init_config(segmenter_conf_t* conf, vod_pool_t* pool);

// get segment count modes
uint32_t segmenter_get_segment_count_last_short(segmenter_conf_t* conf, uint64_t duration_millis);

uint32_t segmenter_get_segment_count_last_long(segmenter_conf_t* conf, uint64_t duration_millis);

uint32_t segmenter_get_segment_count_last_rounded(segmenter_conf_t* conf, uint64_t duration_millis);

// key frames
int64_t segmenter_align_to_key_frames(
	align_to_key_frames_context_t* context,
	int64_t offset,
	int64_t limit);

// live window
vod_status_t segmenter_get_live_window(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	bool_t parse_all_clips,
	get_clip_ranges_result_t* clip_ranges,
	uint32_t* base_clip_index);

// manifest duration
uint64_t segmenter_get_total_duration(
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	media_sequence_t* sequences_end,
	uint32_t media_type);

// get segment durations modes
vod_status_t segmenter_get_segment_durations_estimate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	uint32_t media_type,
	segment_durations_t* result);

vod_status_t segmenter_get_segment_durations_accurate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	media_set_t* media_set,
	media_sequence_t* sequence,
	uint32_t media_type,
	segment_durations_t* result);

// get segment index
uint32_t segmenter_get_segment_index_no_discontinuity(
	segmenter_conf_t* conf,
	uint64_t time_millis);

vod_status_t segmenter_get_segment_index_discontinuity(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	uint32_t initial_segment_index,
	media_clip_timing_t* timing,
	uint64_t time_millis,
	uint32_t* result);

// get start end ranges
vod_status_t segmenter_get_start_end_ranges_gop(
	get_clip_ranges_params_t* params,
	get_clip_ranges_result_t* result);

vod_status_t segmenter_get_start_end_ranges_no_discontinuity(
	get_clip_ranges_params_t* params,
	get_clip_ranges_result_t* result);

vod_status_t segmenter_get_start_end_ranges_discontinuity(
	get_clip_ranges_params_t* params,
	get_clip_ranges_result_t* result);

#endif // __SEGMENTER_H__
