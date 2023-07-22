#include "audio_encoder.h"
#include "audio_filter.h"

// constants
#define AUDIO_ENCODER_BITS_PER_SAMPLE (16)

// typedefs
typedef struct
{
	request_context_t* request_context;
	vod_array_t* frames_array;
	AVCodecContext *encoder;
} audio_encoder_state_t;

// globals
static const AVCodec *encoder_codec = NULL;
static bool_t initialized = FALSE;

static char* aac_encoder_names[] = {
	"libfdk_aac",
	"aac",
	NULL
};


static bool_t
audio_encoder_is_format_supported(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
	const enum AVSampleFormat *p;

	for (p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++)
	{
		if (*p == sample_fmt)
		{
			return TRUE;
		}
	}

	return FALSE;
}

void
audio_encoder_process_init(vod_log_t* log)
{
	char** name;

	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avcodec_register_all();
	#endif

	for (name = aac_encoder_names; ; name++)
	{
		if (*name == NULL)
		{
			vod_log_error(VOD_LOG_WARN, log, 0,
				"audio_encoder_process_init: failed to get AAC encoder, audio encoding is disabled. recompile libavcodec with an aac encoder to enable it");
			return;
		}

		encoder_codec = avcodec_find_encoder_by_name(*name);
		if (encoder_codec != NULL)
		{
			vod_log_error(VOD_LOG_INFO, log, 0,
				"audio_encoder_process_init: using aac encoder \"%s\"", *name);
			break;
		}
	}

	if (!audio_encoder_is_format_supported(encoder_codec, AUDIO_ENCODER_INPUT_SAMPLE_FORMAT))
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_encoder_process_init: encoder does not support the required input format, audio encoding is disabled");
		return;
	}

	initialized = TRUE;
}

vod_status_t
audio_encoder_init(
	request_context_t* request_context,
	audio_encoder_params_t* params,
	vod_array_t* frames_array,
	void** result)
{
	audio_encoder_state_t* state;
	AVCodecContext* encoder;
	int avrc;

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_encoder_init: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_encoder_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// init the encoder
	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_encoder_init: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->encoder = encoder;

	encoder->sample_fmt = AUDIO_ENCODER_INPUT_SAMPLE_FORMAT;
	encoder->time_base.num = 1;
	encoder->time_base.den = params->timescale;
	encoder->sample_rate = params->sample_rate;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	av_channel_layout_from_mask(&encoder->ch_layout, params->channel_layout);
#else
	encoder->channels = params->channels;
	encoder->channel_layout = params->channel_layout;
#endif

	encoder->bit_rate = params->bitrate;
	encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_encoder_init: avcodec_open2 failed %d", avrc);
		audio_encoder_free(state);
		return VOD_UNEXPECTED;
	}

	state->request_context = request_context;
	state->frames_array = frames_array;

	*result = state;

	return VOD_OK;
}

void
audio_encoder_free(
	void* context)
{
	audio_encoder_state_t* state = context;

	if (state == NULL)
	{
		return;
	}
	
	avcodec_close(state->encoder);
	av_free(state->encoder);
}

size_t
audio_encoder_get_frame_size(void* context)
{
	audio_encoder_state_t* state = context;

	if ((state->encoder->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0)
	{
		return 0;
	}

	return state->encoder->frame_size;
}

static vod_status_t
audio_encoder_write_packet(
	audio_encoder_state_t* state,
	AVPacket* output_packet)
{
	input_frame_t* cur_frame;
	vod_status_t rc;
	void* data;

	rc = audio_filter_alloc_memory_frame(
		state->request_context,
		state->frames_array,
		output_packet->size,
		&cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	data = (void*)(uintptr_t)cur_frame->offset;
	vod_memcpy(data, output_packet->data, output_packet->size);

	cur_frame->duration = output_packet->duration;
	cur_frame->pts_delay = output_packet->pts - output_packet->dts;

	return VOD_OK;
}

vod_status_t
audio_encoder_write_frame(
	void* context,
	AVFrame* frame)
{
	audio_encoder_state_t* state = context;
	vod_status_t rc;
	AVPacket* output_packet;
	int avrc;

	// send frame
	avrc = avcodec_send_frame(state->encoder, frame);

	av_frame_unref(frame);

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// receive packet
	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	// packet data will be allocated by the encoder

	avrc = avcodec_receive_packet(state->encoder, output_packet);

	if (avrc == AVERROR(EAGAIN))
	{
		av_packet_free(&output_packet);
		return VOD_OK;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: avcodec_receive_packet failed %d", avrc);
		av_packet_free(&output_packet);
		return VOD_ALLOC_FAILED;
	}

	rc = audio_encoder_write_packet(state, output_packet);

	av_packet_free(&output_packet);

	return rc;
}

vod_status_t
audio_encoder_flush(
	void* context)
{
	audio_encoder_state_t* state = context;
	AVPacket* output_packet;
	vod_status_t rc;
	int avrc;

	avrc = avcodec_send_frame(state->encoder, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_flush: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_flush: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	for (;;)
	{
		// packet data will be allocated by the encoder, av_packet_unref is always called
		avrc = avcodec_receive_packet(state->encoder, output_packet);
		if (avrc == AVERROR_EOF)
		{
			break;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_encoder_flush: avcodec_receive_packet failed %d", avrc);
			av_packet_free(&output_packet);
			return VOD_UNEXPECTED;
		}

		rc = audio_encoder_write_packet(state, output_packet);

		if (rc != VOD_OK)
		{
			av_packet_free(&output_packet);
			return rc;
		}
	}

	av_packet_free(&output_packet);
	return VOD_OK;
}

vod_status_t
audio_encoder_update_media_info(
	void* context,
	media_info_t* media_info)
{
	audio_encoder_state_t* state = context;
	AVCodecContext *encoder = state->encoder;
	u_char* new_extra_data;

	if (encoder->time_base.num != 1)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_update_media_info: unexpected encoder time base %d/%d",
			encoder->time_base.num, encoder->time_base.den);
		return VOD_UNEXPECTED;
	}

	media_info->timescale = encoder->time_base.den;
	media_info->bitrate = encoder->bit_rate;

	media_info->u.audio.object_type_id = 0x40;		// ffmpeg always writes 0x40 (ff_mp4_obj_type)

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	media_info->u.audio.channels = encoder->ch_layout.nb_channels;
	media_info->u.audio.channel_layout = encoder->ch_layout.u.mask;
#else
	media_info->u.audio.channels = encoder->channels;
	media_info->u.audio.channel_layout = encoder->channel_layout;
#endif

	media_info->u.audio.bits_per_sample = AUDIO_ENCODER_BITS_PER_SAMPLE;
	media_info->u.audio.packet_size = 0;			// ffmpeg always writes 0 (mov_write_audio_tag)
	media_info->u.audio.sample_rate = encoder->sample_rate;

	new_extra_data = vod_alloc(state->request_context->pool, encoder->extradata_size);
	if (new_extra_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_encoder_update_media_info: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(new_extra_data, encoder->extradata, encoder->extradata_size);

	media_info->extra_data.data = new_extra_data;
	media_info->extra_data.len = encoder->extradata_size;

	return VOD_OK;
}
