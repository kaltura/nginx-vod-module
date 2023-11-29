#include "thumb_grabber.h"
#include "../media_set.h"

#include <libavcodec/avcodec.h>

#if (VOD_HAVE_LIB_SW_SCALE)
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#endif // VOD_HAVE_LIB_SW_SCALE

// typedefs
typedef struct
{
	// fixed
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;

	// libavcodec
	AVCodecContext *decoder;
	AVCodecContext *encoder;
	AVFrame *decoded_frame;
	AVPacket *output_packet;
	void* resize_buffer;
	int has_frame;

	// frame state
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	uint32_t skip_count;
	bool_t first_time;
	bool_t frame_started;
	uint64_t dts;
	uint32_t missing_frames;

	// frame buffer state
	uint32_t max_frame_size;
	u_char* frame_buffer;
	uint32_t cur_frame_pos;

} thumb_grabber_state_t;

typedef struct {
	uint32_t codec_id;
	enum AVCodecID av_codec_id;
	const char* name;
} codec_id_mapping_t;

// globals
static const AVCodec *decoder_codec[VOD_CODEC_ID_COUNT];
static const AVCodec *encoder_codec = NULL;

static codec_id_mapping_t codec_mappings[] = {
	{ VOD_CODEC_ID_AVC, AV_CODEC_ID_H264, "h264" },
	{ VOD_CODEC_ID_HEVC, AV_CODEC_ID_H265, "h265" },
	{ VOD_CODEC_ID_VP8, AV_CODEC_ID_VP8, "vp8" },
	{ VOD_CODEC_ID_VP9, AV_CODEC_ID_VP9, "vp9" },
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 89, 100)
	{ VOD_CODEC_ID_AV1, AV_CODEC_ID_AV1, "av1" },
#endif
};

void
thumb_grabber_process_init(vod_log_t* log)
{
	const AVCodec *cur_decoder_codec;
	codec_id_mapping_t* mapping_cur;
	codec_id_mapping_t* mapping_end;

	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avcodec_register_all();
	#endif
	vod_memzero(decoder_codec, sizeof(decoder_codec));

	encoder_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (encoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"thumb_grabber_process_init: failed to get jpeg encoder, thumbnail capture is disabled");
		return;
	}

	mapping_end = codec_mappings + vod_array_entries(codec_mappings);
	for (mapping_cur = codec_mappings; mapping_cur < mapping_end; mapping_cur++)
	{
		cur_decoder_codec = avcodec_find_decoder(mapping_cur->av_codec_id);
		if (cur_decoder_codec == NULL)
		{
			vod_log_error(VOD_LOG_WARN, log, 0,
				"thumb_grabber_process_init: failed to get %s decoder, thumbnail capture is disabled for this codec", 
				mapping_cur->name);
			continue;
		}

		decoder_codec[mapping_cur->codec_id] = cur_decoder_codec;
	}
}

static void
thumb_grabber_free_state(void* context)
{
	thumb_grabber_state_t* state = (thumb_grabber_state_t*)context;

	av_packet_free(&state->output_packet);
	if (state->resize_buffer != NULL)
	{
		av_freep(state->resize_buffer);
	}
	av_frame_free(&state->decoded_frame);
	avcodec_close(state->encoder);
	av_free(state->encoder);
	avcodec_close(state->decoder);
	av_free(state->decoder);
}

static vod_status_t
thumb_grabber_init_decoder(
	request_context_t* request_context,
	media_info_t* media_info,
	AVCodecContext** result)
{
	AVCodecContext *decoder;
	int avrc;

	decoder = avcodec_alloc_context3(decoder_codec[media_info->codec_id]);
	if (decoder == NULL) 
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	*result = decoder;

	decoder->codec_tag = media_info->format;
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;
	decoder->pkt_timebase = decoder->time_base;
	decoder->extradata = media_info->extra_data.data;
	decoder->extradata_size = media_info->extra_data.len;
	decoder->width = media_info->u.video.width;
	decoder->height = media_info->u.video.height;

	avrc = avcodec_open2(decoder, decoder_codec[media_info->codec_id], NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

static vod_status_t
thumb_grabber_init_encoder(
	request_context_t* request_context,
	uint32_t width,
	uint32_t height,
	AVCodecContext** result)
{
	AVCodecContext *encoder;
	int avrc;

	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_encoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	*result = encoder;

	encoder->width = width;
	encoder->height = height;
	encoder->time_base = (AVRational){ 1, 1 };
	encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_encoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

static uint32_t
thumb_grabber_get_max_frame_size(media_track_t* track, uint32_t limit)
{
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t max_frame_size = 0;

	part = &track->frames;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame; limit > 0; cur_frame++, limit--)
	{
		if (cur_frame >= last_frame)
		{
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		if (cur_frame->size > max_frame_size)
		{
			max_frame_size = cur_frame->size;
		}
	}

	return max_frame_size;
}

static vod_status_t
thumb_grabber_truncate_frames(
	request_context_t* request_context,
	media_track_t* track, 
	uint64_t requested_time, 
	bool_t accurate,
	uint32_t* skip_count)
{
	frame_list_part_t* last_key_frame_part = NULL;
	frame_list_part_t* min_part = NULL;
	frame_list_part_t* part;
	input_frame_t* last_key_frame = NULL;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	vod_status_t rc;
	uint64_t dts = track->clip_start_time + track->first_frame_time_offset;
	uint64_t pts;
	uint64_t cur_diff;
	uint64_t min_diff = ULLONG_MAX;
	uint32_t last_key_frame_index = 0;
	uint32_t min_index = 0;
	uint32_t index;

	if (track->frame_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_truncate_frames: did not find any frames (1)");
		return VOD_BAD_REQUEST;
	}

	part = &track->frames;
	last_frame = part->last_frame;
	cur_frame = part->first_frame;

	requested_time += cur_frame->pts_delay;

	for (index = 0;; cur_frame++, index++)
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

		// update last_key_frame
		if (cur_frame->key_frame)
		{
			last_key_frame_index = index;
			last_key_frame = cur_frame;
			last_key_frame_part = part;
		}

		// find the closest frame
		pts = dts + cur_frame->pts_delay;
		cur_diff = (pts >= requested_time) ? (pts - requested_time) : (requested_time - pts);
		if (cur_diff <= min_diff && 
			(cur_frame->key_frame || 
			(accurate && last_key_frame != NULL)))
		{
			min_index = index - last_key_frame_index;
			min_diff = cur_diff;
			min_part = last_key_frame_part;

			rc = min_part->frames_source->skip_frames(
				min_part->frames_source_context,
				last_key_frame - min_part->first_frame);
			if (rc != VOD_OK)
			{
				return rc;
			}

			// truncate any frames before the key frame of the closest frame
			min_part->first_frame = last_key_frame;
		}

		dts += cur_frame->duration;
	}

	if (min_part == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_truncate_frames: did not find any frames (2)");
		return VOD_UNEXPECTED;
	}

	// truncate any parts before the key frame of the closest frame
	track->frames = *min_part;

	*skip_count = min_index;

	return VOD_OK;
}

vod_status_t
thumb_grabber_init_state(
	request_context_t* request_context,
	media_track_t* track, 
	request_params_t* request_params,
	bool_t accurate,
	write_callback_t write_callback,
	void* write_context,
	void** result)
{
	thumb_grabber_state_t* state;
	vod_pool_cleanup_t *cln;
	vod_status_t rc;
	uint32_t output_width;
	uint32_t output_height;
	uint32_t frame_index;

	if (decoder_codec[track->media_info.codec_id] == NULL)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"thumb_grabber_init_state: no decoder was initialized for codec %uD", track->media_info.codec_id);
		return VOD_BAD_REQUEST;
	}

	if (track->media_info.u.video.width <= 0 || track->media_info.u.video.height <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_state: input width/height is zero");
		return VOD_BAD_DATA;
	}

	rc = thumb_grabber_truncate_frames(request_context, track, request_params->segment_time, accurate, &frame_index);
	if (rc != VOD_OK)
	{
		return rc;
	}

	vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
		"thumb_grabber_init_state: frame index is %uD", frame_index);

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"thumb_grabber_init_state: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	// clear all ffmpeg members, so that they will be initialized in case init fails
	state->decoded_frame = NULL;
	state->resize_buffer = NULL;
	state->decoder = NULL;
	state->encoder = NULL;
	state->output_packet = NULL;

	// add to the cleanup pool
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"thumb_grabber_init_state: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = thumb_grabber_free_state;
	cln->data = state;

	rc = thumb_grabber_init_decoder(request_context, &track->media_info, &state->decoder);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (request_params->width != 0)
	{
		output_width = request_params->width;
		if (request_params->height != 0)
		{
			output_height = request_params->height;
		}
		else
		{
			output_height = ((uint64_t)track->media_info.u.video.height * request_params->width) / track->media_info.u.video.width;
		}
	}
	else
	{
		if (request_params->height != 0)
		{
			output_width = ((uint64_t)track->media_info.u.video.width * request_params->height) / track->media_info.u.video.height;
			output_height = request_params->height;
		}
		else
		{
			output_width = track->media_info.u.video.width;
			output_height = track->media_info.u.video.height;
		}
	}

	if (output_width <= 0 || output_height <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_state: output width/height is zero");
		return VOD_BAD_REQUEST;
	}

	// TODO: postpone the initialization of the encoder to after a frame is decoded

	rc = thumb_grabber_init_encoder(request_context, output_width, output_height, &state->encoder);
	if (rc != VOD_OK)
	{
		return rc;
	}

	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_state: av_frame_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->output_packet = av_packet_alloc();
	if (state->output_packet == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_state: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	state->cur_frame_part = track->frames;
	state->cur_frame = track->frames.first_frame;
	state->max_frame_size = thumb_grabber_get_max_frame_size(track, frame_index + 1);
	state->frame_buffer = NULL;
	state->skip_count = frame_index;
	state->cur_frame_pos = 0;
	state->first_time = TRUE;
	state->frame_started = FALSE;
	state->missing_frames = 0;
	state->dts = 0;
	state->has_frame = 0;

	*result = state;

	return VOD_OK;
}

static vod_status_t
thumb_grabber_decode_flush(thumb_grabber_state_t* state)
{
	AVFrame* decoded_frame;
	int avrc;

	avrc = avcodec_send_packet(state->decoder, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_decode_flush: avcodec_send_packet failed %d", avrc);
		return VOD_BAD_DATA;
	}

	for (; state->missing_frames > 0; state->missing_frames--)
	{
		decoded_frame = av_frame_alloc();
		if (decoded_frame == NULL)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"thumb_grabber_decode_flush: av_frame_alloc failed");
			return VOD_ALLOC_FAILED;
		}

		avrc = avcodec_receive_frame(state->decoder, decoded_frame);
		if (avrc == AVERROR_EOF)
		{
			av_frame_free(&decoded_frame);
			break;
		}

		if (avrc < 0)
		{
			av_frame_free(&decoded_frame);
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"thumb_grabber_decode_flush: avcodec_decode_video2 failed %d", avrc);
			return VOD_BAD_DATA;
		}

		av_frame_free(&state->decoded_frame);
		state->decoded_frame = decoded_frame;
		state->has_frame = 1;
	}

	return VOD_OK;
}

static vod_status_t 
thumb_grabber_decode_frame(thumb_grabber_state_t* state, u_char* buffer)
{
	input_frame_t* frame = state->cur_frame;
	AVPacket* input_packet;
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int avrc;

	input_packet = av_packet_alloc();
	if (input_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_decode_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	input_packet->data = buffer;
	input_packet->size = frame->size;
	input_packet->dts = state->dts;
	input_packet->pts = state->dts + frame->pts_delay;
	input_packet->duration = frame->duration;
	input_packet->flags = frame->key_frame ? AV_PKT_FLAG_KEY : 0;
	state->dts += frame->duration;
	
	av_frame_unref(state->decoded_frame);

	state->has_frame = 0;

	frame_end = buffer + frame->size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));

	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_decode_frame: avcodec_send_packet failed %d", avrc);
		return VOD_BAD_DATA;
	}

	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
	if (avrc == AVERROR(EAGAIN))
	{
		state->missing_frames++;
	}
	else if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_decode_frame: avcodec_receive_frame failed %d", avrc);
		return VOD_BAD_DATA;
	}
	else
	{
		state->has_frame = 1;
	}

	vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	return VOD_OK;
}

#if (VOD_HAVE_LIB_SW_SCALE)
static vod_status_t
thumb_grabber_resize_frame(thumb_grabber_state_t* state)
{
	struct SwsContext *sws_ctx = NULL;
	AVFrame* input_frame = state->decoded_frame;
	AVFrame* output_frame = NULL;
	vod_status_t rc;
	int avrc;

	output_frame = av_frame_alloc();
	if (output_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_resize_frame: av_frame_alloc failed");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	output_frame->width = state->encoder->width;
	output_frame->height = state->encoder->height;
	output_frame->format = AV_PIX_FMT_YUV420P;

	sws_ctx = sws_getContext(
		input_frame->width, input_frame->height, input_frame->format,
		output_frame->width, output_frame->height, output_frame->format,
		SWS_BICUBIC, NULL, NULL, NULL);
	if (sws_ctx == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_resize_frame: sws_getContext failed");
		rc = VOD_UNEXPECTED;
		goto end;
	}

	avrc = av_image_alloc(
		output_frame->data, output_frame->linesize,
		output_frame->width, output_frame->height, output_frame->format, 16);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_resize_frame: av_image_alloc failed");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	state->resize_buffer = &output_frame->data[0];

	sws_scale(sws_ctx,
		(const uint8_t* const*)input_frame->data, input_frame->linesize, 0, input_frame->height,
		output_frame->data, output_frame->linesize);

	av_frame_free(&state->decoded_frame);
	state->decoded_frame = output_frame;
	output_frame = NULL;
	rc = VOD_OK;

end:

	sws_freeContext(sws_ctx);
	av_frame_free(&output_frame);
	return rc;
}
#endif // VOD_HAVE_LIB_SW_SCALE

static vod_status_t
thumb_grabber_write_frame(thumb_grabber_state_t* state)
{
	vod_status_t rc;
	int avrc;

	if (state->missing_frames > 0)
	{
		rc = thumb_grabber_decode_flush(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	if (!state->has_frame)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_write_frame: no frames were decoded");
		return VOD_UNEXPECTED;
	}

#if (VOD_HAVE_LIB_SW_SCALE)
	if (state->encoder->width != state->decoded_frame->width ||
		state->encoder->height != state->decoded_frame->height)
	{
		rc = thumb_grabber_resize_frame(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
#endif // VOD_HAVE_LIB_SW_SCALE

	avrc = avcodec_send_frame(state->encoder, state->decoded_frame);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_write_frame: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	avrc = avcodec_receive_packet(state->encoder, state->output_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_write_frame: avcodec_receive_packet failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	rc = state->write_callback(state->write_context, state->output_packet->data, state->output_packet->size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

vod_status_t
thumb_grabber_process(void* context)
{
	thumb_grabber_state_t* state = context;
	u_char* read_buffer;
	uint32_t read_size;
	bool_t processed_data = FALSE;
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		// start a frame if needed
		if (!state->frame_started)
		{
			if (state->cur_frame >= state->cur_frame_part.last_frame)
			{
				state->cur_frame_part = *state->cur_frame_part.next;
				state->cur_frame = state->cur_frame_part.first_frame;
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

			if (!processed_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"thumb_grabber_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}

		processed_data = TRUE;

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
						"thumb_grabber_process: vod_alloc failed");
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

		// decode the frame
		rc = thumb_grabber_decode_frame(state, read_buffer);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// if the target frame was reached, write it
		if (state->skip_count <= 0)
		{
			return thumb_grabber_write_frame(state);
		}

		state->skip_count--;

		// move to the next frame
		state->cur_frame++;
		state->frame_started = FALSE;
	}
}
