#ifndef __MEDIA_FILTER_H__
#define __MEDIA_FILTER_H__

// includes
#include "../common.h"

// typedefs
enum {
	MEDIA_FILTER_MPEGTS,
	MEDIA_FILTER_MP4_TO_ANNEXB,
	MEDIA_FILTER_JOINER,
	MEDIA_FILTER_BUFFER,
	MEDIA_FILTER_ADTS,
	MEDIA_FILTER_ENCRYPT,
	MEDIA_FILTER_EAC3_ENCRYPT,
	MEDIA_FILTER_ID3,

	MEDIA_FILTER_COUNT
};

typedef struct {
	request_context_t* request_context;
	void* context[MEDIA_FILTER_COUNT];
} media_filter_context_t;

typedef struct {
	uint64_t pts;
	uint64_t dts;
	int key;
	uint32_t size;
	uint32_t header_size;
} output_frame_t;

typedef vod_status_t (*media_filter_start_frame_t)(
	media_filter_context_t* context, 
	output_frame_t* frame);
typedef vod_status_t (*media_filter_write_t)(
	media_filter_context_t* context, 
	const u_char* buffer, 
	uint32_t size);
typedef vod_status_t (*media_filter_flush_frame_t)(
	media_filter_context_t* context, 
	bool_t last_stream_frame);

typedef void (*media_filter_simulated_start_frame_t)(
	media_filter_context_t* context, 
	output_frame_t* frame);
typedef void (*media_filter_simulated_write_t)(
	media_filter_context_t* context, 
	uint32_t size);
typedef void (*media_filter_simulated_flush_frame_t)(
	media_filter_context_t* context, 
	bool_t last_stream_frame);

typedef struct {
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;
	media_filter_flush_frame_t flush_frame;

	media_filter_simulated_start_frame_t simulated_start_frame;
	media_filter_simulated_write_t simulated_write;
	media_filter_simulated_flush_frame_t simulated_flush_frame;
} media_filter_t;

#endif // __MEDIA_FILTER_H__
