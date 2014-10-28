#ifndef __MEDIA_FILTER_H__
#define __MEDIA_FILTER_H__

// includes
#include "../common.h"

// typedefs
typedef struct {
	uint64_t pts;
	uint64_t dts;
	unsigned pid;
	unsigned sid;
	unsigned* cc;
	int key;
	uint32_t original_size;
	bool_t last_stream_frame;
} output_frame_t;

typedef struct {
	vod_status_t (*start_frame)(void* context, output_frame_t* frame);
	vod_status_t (*write)(void* context, const u_char* buffer, uint32_t size);
	vod_status_t (*flush_frame)(void* context, int32_t margin_size);
	void (*simulated_write)(void* context, output_frame_t* frame);
} media_filter_t;

#endif // __MEDIA_FILTER_H__
