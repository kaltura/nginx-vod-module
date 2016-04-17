#ifndef __FRAMES_SOURCE_CACHE_H__
#define __FRAMES_SOURCE_CACHE_H__

// includes
#include "frames_source.h"
#include "read_cache.h"

// macros
#define get_frame_part_source_clip(part) 	\
	(part.frames_source == &frames_source_cache ? ((frames_source_cache_state_t*)part.frames_source_context)->req.source : NULL)

// typedefs
typedef struct {
	read_cache_state_t* read_cache_state;
	read_cache_request_t req;
} frames_source_cache_state_t;

// globals
extern frames_source_t frames_source_cache;

// functions
vod_status_t frames_source_cache_init(
	request_context_t* request_context,
	read_cache_state_t* read_cache_state,
	void* source,
	int cache_slot_id,
	void** result);

#endif //__FRAMES_SOURCE_CACHE_H__
