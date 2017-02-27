#ifndef __ID3_ENCODER_FILTER_H__
#define __ID3_ENCODER_FILTER_H__

// includes
#include "media_filter.h"
#include "../media_format.h"
#include "../common.h"

// typedefs
typedef struct {
	u_char file_identifier[4];
	u_char version[1];
	u_char flags[1];
	u_char size[4];
} id3_file_header_t;

typedef struct {
	u_char id[4];
	u_char size[4];
	u_char flags[2];
} id3_frame_header_t;

typedef struct {
	u_char encoding[1];
} id3_text_frame_header_t;

typedef struct {
	id3_file_header_t file_header;
	id3_frame_header_t frame_header;
	id3_text_frame_header_t text_frame_header;
} id3_text_frame_t;

typedef struct {
	// input
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;
	media_filter_simulated_start_frame_t simulated_start_frame;
	media_filter_simulated_write_t simulated_write;

	// fixed
	id3_text_frame_t header;
} id3_encoder_state_t;

// functions
void id3_encoder_init(
	id3_encoder_state_t* state,
	media_filter_t* filter,
	media_filter_context_t* context);

#endif // __ID3_ENCODER_FILTER_H__
