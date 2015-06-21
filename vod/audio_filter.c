#include "audio_filter.h"

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include "codec_config.h"

#define ENCODER_INPUT_SAMPLE_FORMAT (AV_SAMPLE_FMT_S16)
#define ENCODER_BITS_PER_SAMPLE (16)
#define AAC_ENCODER_NAME ("libfdk_aac")

#define BUFFERSRC_ARGS_FORMAT ("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%uxL%Z")
#define MAX_SAMPLE_FORMAT_NAME_LEN (10)
#define BUFFERSRC_FILTER_NAME ("abuffer")
#define BUFFERSINK_FILTER_NAME ("abuffersink")
#define INPUT_FILTER_NAME ("in")
#define OUTPUT_FILTER_NAME ("out")
#define BUFFERSINK_PARAM_SAMPLE_FORMATS ("sample_fmts")
#define BUFFERSINK_PARAM_CHANNEL_LAYOUTS ("channel_layouts")
#define BUFFERSINK_PARAM_SAMPLE_RATES ("sample_rates")
#define ATEMPO_FILTER_DESCRIPTION ("atempo=%d.%d%Z")

// uncomment to save intermediate streams to temporary files
/*
#define AUDIO_FILTER_DEBUG
#define AUDIO_FILTER_DEBUG_FILENAME_INPUT "/tmp/input.aac"
#define AUDIO_FILTER_DEBUG_FILENAME_DECODED "/tmp/decoded.pcm"
#define AUDIO_FILTER_DEBUG_FILENAME_FILTERED "/tmp/filtered.pcm"
*/

typedef struct {
	request_context_t* request_context;
	uint32_t speed_nom;
	uint32_t speed_denom;
	mpeg_stream_metadata_t* output;
	
	AVCodecContext *decoder;
	AVCodecContext *encoder;
	AVFilterGraph *filter_graph;
	AVFilterContext *buffer_src;
	AVFilterContext *buffer_sink;
	AVFrame *decoded_frame;
	AVFrame *filtered_frame;

	vod_array_t frames_array;
	vod_array_t frame_offsets_array;
	uint64_t dts;
} audio_filter_state_t;

static AVFilter *buffersrc_filter = NULL;
static AVFilter *buffersink_filter = NULL;
static AVCodec *decoder_codec = NULL;
static AVCodec *encoder_codec = NULL;
static bool_t initialized = FALSE;

static const uint64_t aac_channel_layout[] = {
	0,
	AV_CH_LAYOUT_MONO,
	AV_CH_LAYOUT_STEREO,
	AV_CH_LAYOUT_SURROUND,
	AV_CH_LAYOUT_4POINT0,
	AV_CH_LAYOUT_5POINT0_BACK,
	AV_CH_LAYOUT_5POINT1_BACK,
	AV_CH_LAYOUT_7POINT1_WIDE_BACK,
};

static bool_t 
audio_filter_is_format_supported(AVCodec *codec, enum AVSampleFormat sample_fmt)
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
audio_filter_process_init(vod_log_t* log)
{
	avcodec_register_all();
	avfilter_register_all();

	buffersrc_filter = avfilter_get_by_name(BUFFERSRC_FILTER_NAME);
	if (buffersrc_filter == NULL)
	{
		vod_log_error(VOD_LOG_ERR, log, 0,
			"audio_filter_process_init: failed to get buffer source filter");
		return;
	}

	buffersink_filter = avfilter_get_by_name(BUFFERSINK_FILTER_NAME);
	if (buffersink_filter == NULL)
	{
		vod_log_error(VOD_LOG_ERR, log, 0,
			"audio_filter_process_init: failed to get buffer sink filter");
		return;
	}

	decoder_codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (decoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_ERR, log, 0,
			"audio_filter_process_init: failed to get AAC decoder");
		return;
	}

	encoder_codec = avcodec_find_encoder_by_name(AAC_ENCODER_NAME);
	if (encoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_ERR, log, 0,
			"audio_filter_process_init: failed to get AAC encoder");
		return;
	}

	if (!audio_filter_is_format_supported(encoder_codec, ENCODER_INPUT_SAMPLE_FORMAT))
	{
		vod_log_error(VOD_LOG_ERR, log, 0,
			"audio_filter_process_init: encoder does not support the required input format");
		return;
	}

	initialized = TRUE;
}

static vod_status_t 
audio_filter_init_filters(
	audio_filter_state_t* state, 
	mpeg_stream_metadata_t* stream_metadata,
	const char *filters_descr)
{
	char filter_args[sizeof(BUFFERSRC_ARGS_FORMAT) + 4 * VOD_INT64_LEN + MAX_SAMPLE_FORMAT_NAME_LEN];
	enum AVSampleFormat out_sample_fmts[2];
	int64_t out_channel_layouts[2];
	int out_sample_rates[2];
	AVFilterInOut *outputs = NULL;
	AVFilterInOut *inputs = NULL;
	int avrc;
	vod_status_t rc;

	// allocate the filter graph
	state->filter_graph = avfilter_graph_alloc();
	if (state->filter_graph == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_graph_alloc failed");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	// create the buffer source
	vod_sprintf((u_char*)filter_args, BUFFERSRC_ARGS_FORMAT,
		state->decoder->time_base.num, 
		state->decoder->time_base.den, 
		state->decoder->sample_rate,
		av_get_sample_fmt_name(state->decoder->sample_fmt), 
		state->decoder->channel_layout);

	avrc = avfilter_graph_create_filter(
		&state->buffer_src, 
		buffersrc_filter, 
		INPUT_FILTER_NAME,
		filter_args, 
		NULL, 
		state->filter_graph);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_graph_create_filter(input) failed %d", avrc);
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	// create the buffer sink
	avrc = avfilter_graph_create_filter(
		&state->buffer_sink, 
		buffersink_filter, 
		OUTPUT_FILTER_NAME,
		NULL, 
		NULL, 
		state->filter_graph);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_graph_create_filter(output) failed %d", avrc);
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	// configure the buffer sink
	out_sample_fmts[0] = ENCODER_INPUT_SAMPLE_FORMAT;
	out_sample_fmts[1] = -1;
	avrc = av_opt_set_int_list(
		state->buffer_sink, 
		BUFFERSINK_PARAM_SAMPLE_FORMATS, 
		out_sample_fmts, 
		-1, 
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: av_opt_set_int_list(sample format) failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	out_channel_layouts[0] = state->decoder->channel_layout;
	out_channel_layouts[1] = -1;
	avrc = av_opt_set_int_list(
		state->buffer_sink, 
		BUFFERSINK_PARAM_CHANNEL_LAYOUTS, 
		out_channel_layouts, 
		-1, 
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: av_opt_set_int_list(channel layouts) failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	out_sample_rates[0] = state->decoder->sample_rate;
	out_sample_rates[1] = -1;
	avrc = av_opt_set_int_list(
		state->buffer_sink, 
		BUFFERSINK_PARAM_SAMPLE_RATES, 
		out_sample_rates, 
		-1, 
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: av_opt_set_int_list(sample rates) failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	// create the filter outputs
	outputs = avfilter_inout_alloc();
	if (outputs == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_inout_alloc failed (1)");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	outputs->name = av_strdup(INPUT_FILTER_NAME);
	outputs->filter_ctx = state->buffer_src;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	// create the filter inputs
	inputs = avfilter_inout_alloc();
	if (inputs == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_inout_alloc failed (2)");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	inputs->name = av_strdup(OUTPUT_FILTER_NAME);
	inputs->filter_ctx = state->buffer_sink;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	// parse the filter description
	avrc = avfilter_graph_parse_ptr(state->filter_graph, filters_descr, &inputs, &outputs, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_graph_parse_ptr failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	// validate and configure the graph
	avrc = avfilter_graph_config(state->filter_graph, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_init_filters: avfilter_graph_config failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	// set the buffer sink frame size
	if ((state->encoder->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE) == 0)
	{
		av_buffersink_set_frame_size(state->buffer_sink, state->encoder->frame_size);
	}

	rc = VOD_OK;

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return rc;
}

vod_status_t
audio_filter_alloc_state(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	void** result)
{
	char filter_desc[sizeof(ATEMPO_FILTER_DESCRIPTION) + 2 * VOD_INT64_LEN];
	audio_filter_state_t* state;
	uint32_t initial_alloc_size;
	mp4a_config_t codec_config;
	vod_status_t rc;
	int avrc;
	
	if (!initialized)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	if (stream_metadata->media_info.speed_denom != 10)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: unexpected speed denom, must be 10");
		return VOD_UNEXPECTED;
	}

	// parse the codec config
	rc = codec_config_mp4a_config_parse(
		request_context, 
		stream_metadata->media_info.extra_data,
		stream_metadata->media_info.extra_data_size,
		&codec_config);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memzero(state, sizeof(*state));
	
	state->request_context = request_context;
	state->speed_nom = stream_metadata->media_info.speed_nom;
	state->speed_denom = stream_metadata->media_info.speed_denom;
	state->output = stream_metadata;

	// init the decoder	
	state->decoder = avcodec_alloc_context3(decoder_codec);
	if (state->decoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avcodec_alloc_context3 failed (1)");
		rc = VOD_ALLOC_FAILED;
		goto error;
	}

	state->decoder->codec_tag = stream_metadata->media_info.format;
	state->decoder->bit_rate = stream_metadata->media_info.bitrate;
	state->decoder->time_base.num = 1;
	// Note: changing the timescale in order to revert the speed change that was applied to the frames while parsing the mp4
	state->decoder->time_base.den = stream_metadata->media_info.timescale / stream_metadata->media_info.speed_nom * stream_metadata->media_info.speed_denom;
	state->decoder->pkt_timebase = state->decoder->time_base;
	state->decoder->extradata = (u_char*)stream_metadata->media_info.extra_data;
	state->decoder->extradata_size = stream_metadata->media_info.extra_data_size;
	state->decoder->channels = stream_metadata->media_info.u.audio.channels;
	state->decoder->bits_per_coded_sample = stream_metadata->media_info.u.audio.bits_per_sample;
	state->decoder->sample_rate = stream_metadata->media_info.u.audio.sample_rate;
	state->decoder->channel_layout = 0;
	if (codec_config.channel_config < vod_array_entries(aac_channel_layout))
	{
		state->decoder->channel_layout = aac_channel_layout[codec_config.channel_config];
	}
	
	avrc = avcodec_open2(state->decoder, decoder_codec, NULL);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avcodec_open2(decoder) failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto error;
	}
	
	// init the encoder
	state->encoder = avcodec_alloc_context3(encoder_codec);
	if (state->encoder == NULL) 
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avcodec_alloc_context3 failed (2)");
		rc = VOD_ALLOC_FAILED;
		goto error;
	}

	state->encoder->sample_fmt = ENCODER_INPUT_SAMPLE_FORMAT;
	state->encoder->sample_rate = state->decoder->sample_rate;
	state->encoder->channel_layout = state->decoder->channel_layout;
	state->encoder->channels = state->decoder->channels;
	state->encoder->bit_rate = state->decoder->bit_rate;
	state->encoder->flags |= CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

	avrc = avcodec_open2(state->encoder, encoder_codec, NULL);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avcodec_open2(encoder) failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto error;
	}
	
	// allocate frames
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: av_frame_alloc failed (1)");
		rc = VOD_ALLOC_FAILED;
		goto error;
	}
	state->filtered_frame = av_frame_alloc();
	if (state->filtered_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: av_frame_alloc failed (2)");
		rc = VOD_ALLOC_FAILED;
		goto error;
	}

	// initialize the output arrays
	initial_alloc_size = (stream_metadata->frame_count * stream_metadata->media_info.speed_denom) / stream_metadata->media_info.speed_nom + 10;
	
	if (vod_array_init(&state->frames_array, request_context->pool, initial_alloc_size, sizeof(input_frame_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_array_init failed (1)");
		rc = VOD_ALLOC_FAILED;
		goto error;
	}
	
	if (vod_array_init(&state->frame_offsets_array, request_context->pool, initial_alloc_size, sizeof(uint64_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_array_init failed (2)");
		rc = VOD_ALLOC_FAILED;
		goto error;
	}
	
	// initialize the filter graph
	vod_sprintf((u_char*)filter_desc, 
		ATEMPO_FILTER_DESCRIPTION, 
		(int)(stream_metadata->media_info.speed_nom / 10), 
		(int)(stream_metadata->media_info.speed_nom % 10));

	rc = audio_filter_init_filters(state, stream_metadata, filter_desc);
	if (rc != VOD_OK)
	{
		goto error;
	}
	
	*result = state;
	
	return VOD_OK;

error:

	audio_filter_free_state(state);

	return rc;
}

vod_status_t 
audio_filter_write_frame(audio_filter_state_t* state, AVPacket* output_packet)
{
	input_frame_t* cur_frame;
	uint64_t* cur_offset;
	u_char* new_data;
	
	cur_frame = vod_array_push(&state->frames_array);
	if (cur_frame == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_filter_write_frame: vod_array_push failed (1)");
		return VOD_ALLOC_FAILED;
	}
	
	cur_frame->duration = output_packet->duration;
	cur_frame->size = output_packet->size;
	cur_frame->key_frame = 0;
	cur_frame->pts_delay = output_packet->pts - output_packet->dts;

	cur_offset = vod_array_push(&state->frame_offsets_array);
	if (cur_offset == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_filter_write_frame: vod_array_push failed (2)");
		return VOD_ALLOC_FAILED;
	}
	
	new_data = vod_alloc(state->request_context->pool, output_packet->size);
	if (new_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_filter_write_frame: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(new_data, output_packet->data, output_packet->size);
	*cur_offset = (uint64_t)new_data;
	
	return VOD_OK;
}

vod_status_t 
audio_filter_update_stream_metadata(audio_filter_state_t* state)
{
	mpeg_stream_metadata_t* output = state->output;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t old_timescale;
	vod_status_t rc;
	u_char* new_extra_data;

	if (state->encoder->time_base.num != 1)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_update_stream_metadata: unexpected encoder time base num %d", state->encoder->time_base.num);
		return VOD_UNEXPECTED;
	}

	output->frames = state->frames_array.elts;
	output->frame_count = state->frames_array.nelts;
	output->frame_offsets = state->frame_offsets_array.elts;
	
	output->total_frames_size = 0;
	output->total_frames_duration = 0;
	output->media_info.min_frame_duration = 0;
	output->media_info.max_frame_duration = 0;
	
	last_frame = output->frames + output->frame_count;
	for (cur_frame = output->frames; cur_frame < last_frame; cur_frame++)
	{
		output->total_frames_size += cur_frame->size;
		output->total_frames_duration += cur_frame->duration;
		
		if (cur_frame->duration != 0 && 
			(output->media_info.min_frame_duration == 0 || cur_frame->duration < output->media_info.min_frame_duration))
		{
			output->media_info.min_frame_duration = cur_frame->duration;
		}

		if (cur_frame->duration > output->media_info.max_frame_duration)
		{
			output->media_info.max_frame_duration = cur_frame->duration;
		}
	}
	
	if (output->media_info.min_frame_duration == 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_update_stream_metadata: min frame duration is zero");
		return VOD_UNEXPECTED;
	}

	old_timescale = output->media_info.timescale;
	output->media_info.timescale = state->encoder->time_base.den;
	output->media_info.duration = rescale_time(output->media_info.duration, old_timescale, output->media_info.timescale);
	output->media_info.bitrate = state->encoder->bit_rate;

	output->media_info.speed_nom = 1;
	output->media_info.speed_denom = 1;
	
	output->media_info.u.audio.object_type_id = 0x40;		// ffmpeg always writes 0x40 (ff_mp4_obj_type)
	output->media_info.u.audio.channels = state->encoder->channels;
	output->media_info.u.audio.bits_per_sample = ENCODER_BITS_PER_SAMPLE;
	output->media_info.u.audio.packet_size = 0;				// ffmpeg always writes 0 (mov_write_audio_tag)
	output->media_info.u.audio.sample_rate = state->encoder->sample_rate;
	
	output->key_frame_count = 0;
	output->first_frame_time_offset = rescale_time(output->first_frame_time_offset, old_timescale, output->media_info.timescale);
	
	new_extra_data = vod_alloc(state->request_context->pool, state->encoder->extradata_size);
	if (new_extra_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_filter_update_stream_metadata: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(new_extra_data, state->encoder->extradata, state->encoder->extradata_size);
	
	output->media_info.extra_data = new_extra_data;
	output->media_info.extra_data_size = state->encoder->extradata_size;

	if (output->media_info.codec_name.data != NULL)
	{
		rc = codec_config_get_audio_codec_name(state->request_context, &output->media_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	output->frames_file_index = INVALID_FILE_INDEX;		// the frames are now in memory
	
	// TODO: update raw_atoms
	
	return VOD_OK;
}

static vod_status_t 
audio_filter_flush_encoder(audio_filter_state_t* state)
{
	AVPacket output_packet;
	vod_status_t rc;
	int got_packet;
	int avrc;
	
	for (;;)
	{
		av_init_packet(&output_packet);
		output_packet.data = NULL; // packet data will be allocated by the encoder
		output_packet.size = 0;
		
		got_packet = 0;
		avrc = avcodec_encode_audio2(state->encoder, &output_packet, NULL, &got_packet);
		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_flush_encoder: avcodec_encode_audio2 failed %d", avrc);
			return VOD_UNEXPECTED;
		}
		
		if (!got_packet)
		{
			break;
		}

		rc = audio_filter_write_frame(state, &output_packet);

		av_free_packet(&output_packet);

		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return audio_filter_update_stream_metadata(state);
}

#ifdef AUDIO_FILTER_DEBUG
static void
audio_filter_append_debug_data(const char *path, const void *buffer, size_t size)
{
	FILE* fp;
		
	fp = fopen(path, "a");
	if (fp == NULL)
	{
		return;
	}
	fwrite(buffer, 1, size, fp);
	fclose(fp);
}
#endif // AUDIO_FILTER_DEBUG

vod_status_t 
audio_filter_process_frame(void* context, input_frame_t* frame, u_char* buffer)
{
	audio_filter_state_t* state = (audio_filter_state_t*)context;
	vod_status_t rc;
	AVPacket output_packet;
	AVPacket input_packet;
	int got_packet;
	int got_frame;
	int avrc;
#ifdef AUDIO_FILTER_DEBUG
	size_t data_size;
#endif // AUDIO_FILTER_DEBUG
	
	if (frame == NULL)
	{
		return audio_filter_flush_encoder(state);
	}

#ifdef AUDIO_FILTER_DEBUG
	audio_filter_append_debug_data(AUDIO_FILTER_DEBUG_FILENAME_INPUT, buffer, frame->size);
#endif // AUDIO_FILTER_DEBUG
	
	vod_memzero(&input_packet, sizeof(input_packet));
	input_packet.data = buffer;
	input_packet.size = frame->size;
	input_packet.dts = state->dts;
	input_packet.pts = (state->dts + frame->pts_delay);
	input_packet.duration = frame->duration;
	input_packet.flags = AV_PKT_FLAG_KEY;
	state->dts += frame->duration;
	
	avcodec_get_frame_defaults(state->decoded_frame);

	got_frame = 0;
	avrc = avcodec_decode_audio4(state->decoder, state->decoded_frame, &got_frame, &input_packet);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_process_frame: avcodec_decode_audio4 failed %d", avrc);
		return VOD_BAD_DATA;
	}

	if (!got_frame)
	{
		return VOD_OK;
	}

#ifdef AUDIO_FILTER_DEBUG
	data_size = av_samples_get_buffer_size(
		NULL, 
		state->decoder->channels,
		state->decoded_frame->nb_samples,
		state->decoder->sample_fmt, 
		1);
	audio_filter_append_debug_data(AUDIO_FILTER_DEBUG_FILENAME_DECODED, state->decoded_frame->data[0], data_size);
#endif // AUDIO_FILTER_DEBUG
	
	avrc = av_buffersrc_add_frame_flags(state->buffer_src, state->decoded_frame, AV_BUFFERSRC_FLAG_PUSH);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_process_frame: av_buffersrc_add_frame_flags failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	for (;;)
	{
		avrc = av_buffersink_get_frame_flags(state->buffer_sink, state->filtered_frame, AV_BUFFERSINK_FLAG_NO_REQUEST);
		if (avrc == AVERROR(EAGAIN) || avrc == AVERROR_EOF)
		{
			break;
		}
		
		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_process_frame: av_buffersink_get_frame_flags failed %d", avrc);
			return VOD_UNEXPECTED;
		}

#ifdef AUDIO_FILTER_DEBUG
		data_size = av_samples_get_buffer_size(
			NULL, 
			state->encoder->channels,
			state->filtered_frame->nb_samples,
			state->encoder->sample_fmt, 
			1);
		audio_filter_append_debug_data(AUDIO_FILTER_DEBUG_FILENAME_FILTERED, state->filtered_frame->data[0], data_size);
#endif // AUDIO_FILTER_DEBUG

		av_init_packet(&output_packet);
		output_packet.data = NULL; // packet data will be allocated by the encoder
		output_packet.size = 0;

		got_packet = 0;
		avrc = avcodec_encode_audio2(state->encoder, &output_packet, state->filtered_frame, &got_packet);

		av_frame_unref(state->filtered_frame);

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_process_frame: avcodec_encode_audio2 failed %d", avrc);
			return VOD_ALLOC_FAILED;
		}
		
		if (got_packet)
		{
			rc = audio_filter_write_frame(state, &output_packet);

			av_free_packet(&output_packet);
			
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}
	
	return VOD_OK;
}

void 
audio_filter_free_state(void* context)
{
	audio_filter_state_t* state = (audio_filter_state_t*)context;

	avfilter_graph_free(&state->filter_graph);
	avcodec_close(state->decoder);
	av_free(state->decoder);
	avcodec_close(state->encoder);
	av_free(state->encoder);
	av_frame_free(&state->decoded_frame);
	av_frame_free(&state->filtered_frame);
}

#else

// empty stubs in case libavfilter/libavcodec are missing
void 
audio_filter_process_init(vod_log_t* log)
{
}

vod_status_t 
audio_filter_alloc_state(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	void** result)
{
	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"audio_filter_alloc_state: audio filtering not supported, recompile with avcodec/avfilter to enable it");
	return VOD_UNEXPECTED;
}

vod_status_t 
audio_filter_process_frame(void* context, input_frame_t* frame, u_char* buffer)
{
	return VOD_UNEXPECTED;
}

void audio_filter_free_state(void* context)
{
}

#endif
