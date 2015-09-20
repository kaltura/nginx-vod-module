#ifndef __FRAME_JOINER_H__
#define __FRAME_JOINER_H__

// include
#include "media_filter.h"

// typedefs
typedef struct {
	// input data
	const media_filter_t* next_filter;
	void* next_filter_context;

	uint64_t frame_dts;
} frame_joiner_t;

// globals
extern const media_filter_t frame_joiner;

// functions
void frame_joiner_init(
	frame_joiner_t* state,
	const media_filter_t* next_filter,
	void* next_filter_context);

#endif // __FRAME_JOINER_H__
