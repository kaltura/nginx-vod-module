#ifndef __AUDIO_DECODER_H__
#define __AUDIO_DECODER_H__

// includes
#include "../media_format.h"
#include <libavcodec/avcodec.h>

// macros
#define audio_decoder_has_frame(decoder) \
	((decoder)->cur_frame < (decoder)->cur_frame_part.last_frame)

// typedefs
typedef struct {
	request_context_t* request_context;
	AVCodecContext* decoder;
	AVFrame* decoded_frame;

	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	uint64_t dts;

	u_char* frame_buffer;
	uint32_t max_frame_size;
	uint32_t cur_frame_pos;
	bool_t data_handled;
	bool_t frame_started;
} audio_decoder_state_t;

// functions
void audio_decoder_process_init(vod_log_t* log);

vod_status_t audio_decoder_init(
	audio_decoder_state_t* state,
	request_context_t* request_context,
	media_track_t* track,
	int cache_slot_id);

void audio_decoder_free(audio_decoder_state_t* state);

vod_status_t audio_decoder_get_frame(
	audio_decoder_state_t* state,
	AVFrame** result);

#endif // __AUDIO_DECODER_H__
