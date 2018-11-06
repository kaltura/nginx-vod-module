#ifndef __FRAMES_SOURCE_H__
#define __FRAMES_SOURCE_H__

// includes
#include "read_cache.h"

// typedefs
struct input_frame_s;

typedef struct {
	void(*set_cache_slot_id)(void* context, int cache_slot_id);
	vod_status_t(*start_frame)(void* context, struct input_frame_s* frame, read_cache_hint_t* cache_hint);
	vod_status_t(*read)(void* context, u_char** buffer, uint32_t* size, bool_t* frame_done);
	void(*disable_buffer_reuse)(void* context);
	vod_status_t(*skip_frames)(void* context, uint32_t skip_count);
} frames_source_t;

#endif // __FRAMES_SOURCE_H__
