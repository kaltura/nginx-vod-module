#ifndef __MEDIA_FILTER_H__
#define __MEDIA_FILTER_H__

// includes
#include "../common.h"

// typedefs
typedef struct {
	uint64_t pts;
	uint64_t dts;
	int key;
	uint32_t size;
	uint32_t header_size;
} output_frame_t;

typedef struct {
	vod_status_t (*start_frame)(void* context, output_frame_t* frame);
	vod_status_t (*write)(void* context, const u_char* buffer, uint32_t size);
	vod_status_t (*flush_frame)(void* context, bool_t last_stream_frame);

	void (*simulated_start_frame)(void* context, output_frame_t* frame);
	void (*simulated_write)(void* context, uint32_t size);
	void (*simulated_flush_frame)(void* context, bool_t last_stream_frame);
} media_filter_t;

#endif // __MEDIA_FILTER_H__
