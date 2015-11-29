#ifndef __FRAMES_SOURCE_CACHE_H__
#define __FRAMES_SOURCE_CACHE_H__

// includes
#include "frames_source.h"
#include "read_cache.h"

// globals
extern frames_source_t frames_source_cache;

// functions
vod_status_t frames_source_cache_init(
	request_context_t* request_context,
	read_cache_state_t* read_cache_state,
	void* source,
	void** result);

#endif //__FRAMES_SOURCE_CACHE_H__
