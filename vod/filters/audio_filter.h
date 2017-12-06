#ifndef __AUDIO_FILTER_H__
#define __AUDIO_FILTER_H__

// includes
#include "../media_set.h"

// typedefs
struct audio_filter_s {
	uint32_t(*get_filter_desc_size)(media_clip_t* clip);
	u_char* (*append_filter_desc)(u_char* p, media_clip_t* clip);
};

typedef struct audio_filter_s audio_filter_t;

// functions
void audio_filter_process_init(vod_log_t* log);

vod_status_t audio_filter_alloc_state(
	request_context_t* request_context,
	media_sequence_t* sequence,
	media_clip_t* clip,
	media_track_t* output_track,
	uint32_t max_frame_count,
	uint32_t output_codec_id,
	size_t* cache_buffer_count,
	void** result);

void audio_filter_free_state(void* context);

vod_status_t audio_filter_process(void* context);

vod_status_t audio_filter_alloc_memory_frame(
	request_context_t* request_context,
	vod_array_t* frames_array,
	size_t size,
	input_frame_t** result);

#endif // __AUDIO_FILTER_H__
