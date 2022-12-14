#include "audio_decoder.h"

// globals
static const AVCodec *decoder_codec = NULL;
static bool_t initialized = FALSE;

void
audio_decoder_process_init(vod_log_t* log)
{
	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avcodec_register_all();
	#endif

	decoder_codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (decoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_decoder_process_init: failed to get AAC decoder, audio decoding is disabled");
		return;
	}

	initialized = TRUE;
}

static vod_status_t
audio_decoder_init_decoder(
	audio_decoder_state_t* state,
	media_info_t* media_info)
{
	AVCodecContext* decoder;
	int avrc;

	if (media_info->codec_id != VOD_CODEC_ID_AAC)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: codec id %uD not supported", media_info->codec_id);
		return VOD_BAD_REQUEST;
	}

	// init the decoder	
	decoder = avcodec_alloc_context3(decoder_codec);
	if (decoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->decoder = decoder;	
	
	decoder->codec_tag = media_info->format;
	decoder->bit_rate = media_info->bitrate;
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;
	decoder->pkt_timebase = decoder->time_base;
	decoder->extradata = media_info->extra_data.data;
	decoder->extradata_size = media_info->extra_data.len;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	av_channel_layout_from_mask(&decoder->ch_layout, media_info->u.audio.channel_layout);
#else
	decoder->channels = media_info->u.audio.channels;
	decoder->channel_layout = media_info->u.audio.channel_layout;
#endif

	decoder->bits_per_coded_sample = media_info->u.audio.bits_per_sample;
	decoder->sample_rate = media_info->u.audio.sample_rate;

	avrc = avcodec_open2(decoder, decoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
audio_decoder_init(
	audio_decoder_state_t* state,
	request_context_t* request_context,
	media_track_t* track,
	int cache_slot_id)
{
	frame_list_part_t* part;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	vod_status_t rc;

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_decoder_init: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	state->request_context = request_context;

	// init the decoder
	rc = audio_decoder_init_decoder(
		state,
		&track->media_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate a frame
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_decoder_init: av_frame_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// calculate the max frame size
	state->max_frame_size = 0;
	part = &track->frames;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame;; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		if (cur_frame->size > state->max_frame_size)
		{
			state->max_frame_size = cur_frame->size;
		}
	}

	// initialize the frame state
	state->cur_frame_pos = 0;
	state->data_handled = TRUE;
	state->frame_started = FALSE;
	state->frame_buffer = NULL;

	state->cur_frame_part = track->frames;
	state->cur_frame = track->frames.first_frame;
	state->dts = track->first_frame_time_offset;

	state->cur_frame_part.frames_source->set_cache_slot_id(
		state->cur_frame_part.frames_source_context,
		cache_slot_id);

	return VOD_OK;
}

void
audio_decoder_free(audio_decoder_state_t* state)
{
	avcodec_close(state->decoder);
	av_free(state->decoder);
	state->decoder = NULL;
	av_frame_free(&state->decoded_frame);
}

static vod_status_t
audio_decoder_decode_frame(
	audio_decoder_state_t* state,
	u_char* buffer,
	AVFrame** result)
{
	input_frame_t* frame = state->cur_frame;
	AVPacket* input_packet;
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int avrc;

	input_packet = av_packet_alloc();
	if (input_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_decode_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// send a frame
	input_packet->data = buffer;
	input_packet->size = frame->size;
	input_packet->dts = state->dts;
	input_packet->pts = state->dts + frame->pts_delay;
	input_packet->duration = frame->duration;
	input_packet->flags = AV_PKT_FLAG_KEY;
	state->dts += frame->duration;

	av_frame_unref(state->decoded_frame);

	frame_end = buffer + frame->size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));

	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_decode_frame: avcodec_send_packet failed %d", avrc);
		return VOD_BAD_DATA;
	}

	// move to the next frame
	state->cur_frame++;
	if (state->cur_frame >= state->cur_frame_part.last_frame &&
		state->cur_frame_part.next != NULL)
	{
		state->cur_frame_part = *state->cur_frame_part.next;
		state->cur_frame = state->cur_frame_part.first_frame;
	}

	state->frame_started = FALSE;

	// receive a frame
	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);

	vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	if (avrc == AVERROR(EAGAIN))
	{
		return VOD_AGAIN;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_decode_frame: avcodec_receive_frame failed %d", avrc);
		return VOD_BAD_DATA;
	}

	*result = state->decoded_frame;
	return VOD_OK;
}

vod_status_t
audio_decoder_get_frame(
	audio_decoder_state_t* state,
	AVFrame** result)
{
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		// start a frame if needed
		if (!state->frame_started)
		{
			if (state->cur_frame >= state->cur_frame_part.last_frame)
			{
				return VOD_DONE;
			}

			// start the frame
			rc = state->cur_frame_part.frames_source->start_frame(
				state->cur_frame_part.frames_source_context,
				state->cur_frame,
				NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->frame_started = TRUE;
		}

		// read some data from the frame
		rc = state->cur_frame_part.frames_source->read(
			state->cur_frame_part.frames_source_context,
			&read_buffer,
			&read_size,
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!state->data_handled)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"audio_decoder_get_frame: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->data_handled = FALSE;
			return VOD_AGAIN;
		}

		state->data_handled = TRUE;

		if (!frame_done)
		{
			// didn't finish the frame, append to the frame buffer
			if (state->frame_buffer == NULL)
			{
				state->frame_buffer = vod_alloc(
					state->request_context->pool,
					state->max_frame_size + VOD_BUFFER_PADDING_SIZE);
				if (state->frame_buffer == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
						"audio_decoder_get_frame: vod_alloc failed");
					return VOD_ALLOC_FAILED;
				}
			}

			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos += read_size;
			continue;
		}

		if (state->cur_frame_pos != 0)
		{
			// copy the remainder
			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos = 0;
			read_buffer = state->frame_buffer;
		}

		// process the frame
		rc = audio_decoder_decode_frame(state, read_buffer, result);
		if (rc != VOD_AGAIN)
		{
			return rc;
		}
	}
}
