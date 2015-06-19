#ifndef __HDS_FRAGMENT_H__
#define __HDS_FRAGMENT_H__

// includes
#include "../mp4/mp4_builder.h"
#include "../mp4/mp4_parser.h"
#include "../common.h"

// constants
#define HDS_TIMESCALE (1000)

// typedefs
struct hds_muxer_state_s;
typedef struct hds_muxer_state_s hds_muxer_state_t;

// functions
vod_status_t hds_muxer_init_fragment(
	request_context_t* request_context,
	uint32_t segment_index,
	mpeg_metadata_t *mpeg_metadata,
	read_cache_state_t* read_cache_state,
	write_callback_t write_callback,
	void* write_context,
	bool_t size_only,
	vod_str_t* header,
	size_t* total_fragment_size,
	hds_muxer_state_t** processor_state);

vod_status_t hds_muxer_process_frames(hds_muxer_state_t* state);

#endif // __HDS_FRAGMENT_H__
