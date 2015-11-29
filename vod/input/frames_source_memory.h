#ifndef __FRAMES_SOURCE_MEMORY_H__
#define __FRAMES_SOURCE_MEMORY_H__

// includes
#include "frames_source.h"

// globals
extern frames_source_t frames_source_memory;

// functions
vod_status_t frames_source_memory_init(
	request_context_t* request_context,
	void** result);

#endif //__FRAMES_SOURCE_MEMORY_H__
