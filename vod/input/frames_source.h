#ifndef __FRAMES_SOURCE_H__
#define __FRAMES_SOURCE_H__

// includes
#include "../common.h"

// typedefs
struct input_frame_s;

typedef struct {
	void(*set_cache_slot_id)(void* context, int cache_slot_id);
	vod_status_t(*start_frame)(void* context, struct input_frame_s* frame, uint64_t min_offset);
	vod_status_t(*read)(void* context, u_char** buffer, uint32_t* size, bool_t* frame_done);
	void(*disable_buffer_reuse)(void* context);
} frames_source_t;

#endif // __FRAMES_SOURCE_H__
