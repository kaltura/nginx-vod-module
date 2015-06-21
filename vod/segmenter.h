#ifndef __SEGMENTER_H__
#define __SEGMENTER_H__

// includes
#include "mp4/mp4_parser.h"
#include "common.h"

// constants
#define INVALID_SEGMENT_COUNT UINT_MAX

// typedefs
struct segmenter_conf_s;
typedef struct segmenter_conf_s segmenter_conf_t;

typedef struct {
	uint32_t segment_index;
	uint32_t repeat_count;
	uint64_t duration;
} segment_duration_item_t;

typedef struct {
	segment_duration_item_t* items;
	uint32_t item_count;
	uint32_t segment_count;
	uint32_t timescale;
	uint32_t duration_millis;
} segment_durations_t;

typedef uint32_t(*segmenter_get_segment_count_t)(segmenter_conf_t* conf, uint32_t duration_millis);

typedef vod_status_t (*segmenter_get_segment_durations_t)(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	mpeg_stream_metadata_t** streams,
	uint32_t stream_count,
	segment_durations_t* result);

struct segmenter_conf_s {
	// config fields
	uintptr_t segment_duration;
	vod_array_t* bootstrap_segments;		// array of vod_str_t
	bool_t align_to_key_frames;
	segmenter_get_segment_count_t get_segment_count;			// last short / last long / last rounded
	segmenter_get_segment_durations_t get_segment_durations;	// estimate / accurate

	// derived fields
	uint32_t parse_type;
	uint32_t bootstrap_segments_count;
	uint32_t* bootstrap_segments_durations;
	uint32_t max_segment_duration;
	uint32_t bootstrap_segments_total_duration;
	uint32_t* bootstrap_segments_start;
	uint32_t* bootstrap_segments_mid;
	uint32_t* bootstrap_segments_end;
};

typedef struct {
	segmenter_conf_t* conf;
	uint32_t segment_index;
	uint32_t segment_count;
	uint32_t last_boundary;
} segmenter_boundary_iterator_context_t;

// functions
vod_status_t segmenter_init_config(segmenter_conf_t* conf, vod_pool_t* pool);

uint32_t segmenter_get_segment_count_last_short(segmenter_conf_t* conf, uint32_t duration_millis);

uint32_t segmenter_get_segment_count_last_long(segmenter_conf_t* conf, uint32_t duration_millis);

uint32_t segmenter_get_segment_count_last_rounded(segmenter_conf_t* conf, uint32_t duration_millis);

void segmenter_get_start_end_offsets(segmenter_conf_t* conf, uint32_t segment_index, uint64_t* start, uint64_t* end);

uint32_t segmenter_get_segment_index(segmenter_conf_t* conf, uint32_t time_millis);

vod_status_t
segmenter_get_segment_durations_estimate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	mpeg_stream_metadata_t** streams,
	uint32_t stream_count,
	segment_durations_t* result);

vod_status_t
segmenter_get_segment_durations_accurate(
	request_context_t* request_context,
	segmenter_conf_t* conf,
	mpeg_stream_metadata_t** streams,
	uint32_t stream_count,
	segment_durations_t* result);

void segmenter_boundary_iterator_init(segmenter_boundary_iterator_context_t* context, segmenter_conf_t* conf, uint32_t segment_count);

uint32_t segmenter_boundary_iterator_next(segmenter_boundary_iterator_context_t* context);

void segmenter_boundary_iterator_skip(segmenter_boundary_iterator_context_t* context, uint32_t count);

#endif // __SEGMENTER_H__
