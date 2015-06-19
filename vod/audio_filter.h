#ifndef __AUDIO_FILTER_H__
#define __AUDIO_FILTER_H__

// includes
#include "mp4/mp4_parser.h"

// functions
void audio_filter_process_init(vod_log_t* log);

vod_status_t audio_filter_alloc_state(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	void** result);

vod_status_t audio_filter_process_frame(void* context, input_frame_t* frame, u_char* buffer);

void audio_filter_free_state(void* context);

#endif // __AUDIO_FILTER_H__
