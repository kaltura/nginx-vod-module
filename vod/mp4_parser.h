#ifndef __MP4_PARSER_H__
#define __MP4_PARSER_H__

// includes
#include "common.h"

// constants
#define MAX_FRAME_SIZE (10 * 1024 * 1024)

// enums
enum {
	MEDIA_TYPE_NONE,
	MEDIA_TYPE_VIDEO,
	MEDIA_TYPE_AUDIO,
	MEDIA_TYPE_COUNT,
};

enum {
	PARSE_SEGMENT,
	PARSE_IFRAMES,
	PARSE_INDEX,
};

// typedefs
typedef struct {
	int64_t dts;
	int64_t pts;
	uint32_t size;
	uint32_t key_frame;
} input_frame_t;

typedef struct {
	int media_type;
	uint32_t track_index;
	input_frame_t* frames;
	uint64_t* frame_offsets;		// Saved outside input_frame_t since it's not needed for iframes file
	uint32_t frame_count;
	uint32_t key_frame_count;
	u_char* extra_data;
	uint32_t extra_data_size;
	int64_t duration;
} mpeg_stream_metadata_t;

typedef struct {
	vod_array_t streams;
} mpeg_metadata_t;

// functions
vod_status_t get_moov_atom_info(
	request_context_t* request_context, 
	const u_char* buffer, 
	size_t buffer_size, 
	off_t* offset, 
	size_t* size);

vod_status_t mp4_parser_parse_moov_atom(
	request_context_t* request_context, 
	uint32_t* required_tracks_mask,
	const u_char* buffer,
	size_t size, 
	mpeg_metadata_t* mpeg_metadata);

#endif // __MP4_PARSER_H__
