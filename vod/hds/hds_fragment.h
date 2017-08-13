#ifndef __HDS_FRAGMENT_H__
#define __HDS_FRAGMENT_H__

// includes
#include "../media_format.h"
#include "../media_set.h"
#include "../common.h"
#include "hds_encryption.h"

// constants
#define HDS_TIMESCALE (1000)

// macros
#define hds_rescale_millis(millis) (millis)

// typedefs
struct hds_muxer_state_s;
typedef struct hds_muxer_state_s hds_muxer_state_t;

typedef struct {
	bool_t generate_moof_atom;
} hds_fragment_config_t;

// functions
vod_status_t hds_muxer_init_fragment(
	request_context_t* request_context,
	hds_fragment_config_t* conf,
	hds_encryption_params_t* encryption_params,
	uint32_t segment_index,
	media_set_t* media_set,
	write_callback_t write_callback,
	void* write_context,
	bool_t size_only,
	vod_str_t* header,
	size_t* total_fragment_size,
	hds_muxer_state_t** processor_state);

vod_status_t hds_muxer_process_frames(hds_muxer_state_t* state);

#endif // __HDS_FRAGMENT_H__
