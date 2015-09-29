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

typedef vod_status_t (*media_filter_start_frame_t)(void* context, output_frame_t* frame);
typedef vod_status_t (*media_filter_write_t)(void* context, const u_char* buffer, uint32_t size);
typedef vod_status_t (*media_filter_flush_frame_t)(void* context, bool_t last_stream_frame);

typedef void (*media_filter_simulated_start_frame_t)(void* context, output_frame_t* frame);
typedef void (*media_filter_simulated_write_t)(void* context, uint32_t size);
typedef void (*media_filter_simulated_flush_frame_t)(void* context, bool_t last_stream_frame);

typedef struct {
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;
	media_filter_flush_frame_t flush_frame;

	media_filter_simulated_start_frame_t simulated_start_frame;
	media_filter_simulated_write_t simulated_write;
	media_filter_simulated_flush_frame_t simulated_flush_frame;
} media_filter_t;

#endif // __MEDIA_FILTER_H__
