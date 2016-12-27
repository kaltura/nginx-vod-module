#include "audio_filter.h"
#include "rate_filter.h"

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include "../input/frames_source_memory.h"

// constants
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

#define MAX_FRAME_COUNT (65536)

// uncomment to save intermediate streams to temporary files
/*
#define AUDIO_FILTER_DEBUG
#define AUDIO_FILTER_DEBUG_PATH "/tmp/"
*/

// typedefs
typedef struct
{
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	uint64_t dts;

	AVCodecContext *decoder;
	AVFilterContext *buffer_src;
} audio_filter_source_t;

typedef struct
{
	AVFilterContext *buffer_sink;
	AVCodecContext *encoder;
} audio_filter_sink_t;

#endif

typedef struct {
	// phase 1
	request_context_t* request_context;
	uint32_t graph_desc_size;
	uint32_t source_count;
	uint32_t output_frame_count;

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)
	// phase 2
	AVFilterGraph *filter_graph;
	AVFilterInOut** outputs;
	u_char* graph_desc;
	u_char* graph_desc_pos;
	audio_filter_source_t* cur_source;
	uint32_t max_frame_size;
	int cache_slot_id;
#endif
} audio_filter_init_context_t;

static vod_status_t 
audio_filter_walk_filters_prepare_init(
	audio_filter_init_context_t* state, 
	media_clip_t** clip_ptr, 
	uint32_t speed_num, 
	uint32_t speed_denom)
{
	media_clip_rate_filter_t* rate_filter;
	media_clip_source_t* source;
	media_track_t* audio_track;
	media_track_t* cur_track;
	media_clip_t** sources_end;
	media_clip_t** sources_cur;
	media_clip_t* clip = *clip_ptr;
	media_clip_t* last_source = NULL;
	vod_status_t rc;
	uint32_t cur_frame_count;
	uint32_t source_count;

	switch (clip->type)
	{
	case MEDIA_CLIP_SOURCE:
		source = vod_container_of(clip, media_clip_source_t, base);

		audio_track = NULL;
		for (cur_track = source->track_array.first_track; cur_track < source->track_array.last_track; cur_track++)
		{
			if (cur_track->media_info.media_type != MEDIA_TYPE_AUDIO)
			{
				continue;
			}

			if (audio_track != NULL)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"audio_filter_walk_filters_prepare_init: more than one audio track per source - unsupported");
				return VOD_BAD_REQUEST;
			}

			audio_track = cur_track;
		}

		if (audio_track == NULL || audio_track->frame_count == 0)
		{
			*clip_ptr = NULL;
			return VOD_OK;
		}

		state->source_count++;

		cur_frame_count = ((uint64_t)audio_track->frame_count * speed_denom) / speed_num;
		if (state->output_frame_count < cur_frame_count)
		{
			state->output_frame_count = cur_frame_count;
		}
		return VOD_OK;

	case MEDIA_CLIP_RATE_FILTER:
		rate_filter = vod_container_of(clip, media_clip_rate_filter_t, base);
		speed_num = ((uint64_t)speed_num * rate_filter->rate.num) / rate_filter->rate.denom;
		break;

	default:;
	}

	// recursively prepare the child sources
	source_count = 0;

	sources_end = clip->sources + clip->source_count;
	for (sources_cur = clip->sources; sources_cur < sources_end; sources_cur++)
	{
		rc = audio_filter_walk_filters_prepare_init(state, sources_cur, speed_num, speed_denom);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (*sources_cur != NULL)
		{
			source_count++;
			last_source = *sources_cur;
		}
	}

	// skip the current filter when it's not needed
	switch (source_count)
	{
	case 0:
		*clip_ptr = NULL;
		return VOD_OK;

	case 1:
		switch (clip->type)
		{
		case MEDIA_CLIP_MIX_FILTER:
		case MEDIA_CLIP_CONCAT:
			// in case of mixing a single clip or concat, skip the filter
			*clip_ptr = last_source;
			return VOD_OK;

		default:;
		}
		break;
	}

	// update the graph description size
	state->graph_desc_size += clip->audio_filter->get_filter_desc_size(clip) + 1;	// 1 = ';'

	return VOD_OK;
}

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)

typedef struct {
	request_context_t* request_context;
	
	// ffmpeg filter
	AVFilterGraph *filter_graph;
	AVFrame *decoded_frame;
	AVFrame *filtered_frame;

	// filter end points
	audio_filter_sink_t sink;
	audio_filter_source_t* sources;
	audio_filter_source_t* sources_end;

	// output
	media_sequence_t* sequence;
	media_track_t* output;
	vod_array_t frames_array;

	// processing state
	audio_filter_source_t* cur_source;
	u_char* frame_buffer;
	uint32_t max_frame_size;
	uint32_t cur_frame_pos;
	bool_t first_time;
} audio_filter_state_t;

// globals
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
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: failed to get buffer source filter, audio filtering is disabled");
		return;
	}

	buffersink_filter = avfilter_get_by_name(BUFFERSINK_FILTER_NAME);
	if (buffersink_filter == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: failed to get buffer sink filter, audio filtering is disabled");
		return;
	}

	decoder_codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (decoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: failed to get AAC decoder, audio filtering is disabled");
		return;
	}

	encoder_codec = avcodec_find_encoder_by_name(AAC_ENCODER_NAME);
	if (encoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: failed to get AAC encoder, audio filtering is disabled. recompile libavcodec with libfdk_aac to enable it");
		return;
	}

	if (!audio_filter_is_format_supported(encoder_codec, ENCODER_INPUT_SAMPLE_FORMAT))
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: encoder does not support the required input format, audio filtering is disabled");
		return;
	}

	initialized = TRUE;
}

static vod_status_t
audio_filter_init_source(
	request_context_t* request_context,
	AVFilterGraph *filter_graph,
	const u_char* source_name,
	media_info_t* media_info,
	audio_filter_source_t* source, 
	AVFilterInOut** outputs)
{
	char filter_args[sizeof(BUFFERSRC_ARGS_FORMAT) + 4 * VOD_INT64_LEN + MAX_SAMPLE_FORMAT_NAME_LEN];
	AVCodecContext* decoder;
	AVFilterInOut* output_link;
	uint8_t channel_config;
	int avrc;

	if (media_info->codec_id != VOD_CODEC_ID_AAC)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: codec id %uD not supported", media_info->codec_id);
		return VOD_BAD_REQUEST;
	}

	// init the decoder	
	decoder = avcodec_alloc_context3(decoder_codec);
	if (decoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	source->decoder = decoder;

	decoder->codec_tag = media_info->format;
	decoder->bit_rate = media_info->bitrate;
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;
	decoder->pkt_timebase = decoder->time_base;
	decoder->extradata = media_info->extra_data.data;
	decoder->extradata_size = media_info->extra_data.len;
	decoder->channels = media_info->u.audio.channels;
	decoder->bits_per_coded_sample = media_info->u.audio.bits_per_sample;
	decoder->sample_rate = media_info->u.audio.sample_rate;
	channel_config = media_info->u.audio.codec_config.channel_config;
	if (channel_config < vod_array_entries(aac_channel_layout))
	{
		decoder->channel_layout = aac_channel_layout[channel_config];
	}
	else
	{
		decoder->channel_layout = 0;
	}

	avrc = avcodec_open2(decoder, decoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// create the buffer source
	vod_sprintf((u_char*)filter_args, BUFFERSRC_ARGS_FORMAT,
		decoder->time_base.num,
		decoder->time_base.den,
		decoder->sample_rate,
		av_get_sample_fmt_name(decoder->sample_fmt),
		decoder->channel_layout);

	avrc = avfilter_graph_create_filter(
		&source->buffer_src,
		buffersrc_filter,
		(char*)source_name,
		filter_args,
		NULL,
		filter_graph);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: avfilter_graph_create_filter failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	// add to the outputs list
	output_link = avfilter_inout_alloc();
	if (output_link == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: avfilter_inout_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	output_link->filter_ctx = source->buffer_src;
	output_link->pad_idx = 0;
	output_link->next = *outputs;
	*outputs = output_link;

	output_link->name = av_strdup((char*)source_name);
	if (output_link->name == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: av_strdup failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

static vod_status_t
audio_filter_init_sink(
	request_context_t* request_context,
	AVFilterGraph *filter_graph,
	media_track_t* reference_track,
	const u_char* sink_name,
	audio_filter_sink_t* sink, 
	AVFilterInOut** inputs)
{
	AVCodecContext* encoder;
	AVFilterInOut* input_link;
	enum AVSampleFormat out_sample_fmts[2];
	int64_t out_channel_layouts[2];
	uint64_t channel_layout;
	uint8_t channel_config;
	int out_sample_rates[2];
	int avrc;

	// Note: matching the output to some reference track, may need to change in the future
	//		if filters such as 'join' will be added

	channel_config = reference_track->media_info.u.audio.codec_config.channel_config;
	if (channel_config < vod_array_entries(aac_channel_layout))
	{
		channel_layout = aac_channel_layout[channel_config];
	}
	else
	{
		channel_layout = 0;
	}

	// create the buffer sink
	avrc = avfilter_graph_create_filter(
		&sink->buffer_sink,
		buffersink_filter,
		(char*)sink_name,
		NULL,
		NULL,
		filter_graph);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avfilter_graph_create_filter failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	// configure the buffer sink
	out_sample_fmts[0] = ENCODER_INPUT_SAMPLE_FORMAT;
	out_sample_fmts[1] = -1;
	avrc = av_opt_set_int_list(
		sink->buffer_sink,
		BUFFERSINK_PARAM_SAMPLE_FORMATS,
		out_sample_fmts,
		-1,
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_int_list(sample formats) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	out_channel_layouts[0] = channel_layout;
	out_channel_layouts[1] = -1;
	avrc = av_opt_set_int_list(
		sink->buffer_sink,
		BUFFERSINK_PARAM_CHANNEL_LAYOUTS,
		out_channel_layouts,
		-1,
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_int_list(channel layouts) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	out_sample_rates[0] = reference_track->media_info.u.audio.sample_rate;
	out_sample_rates[1] = -1;
	avrc = av_opt_set_int_list(
		sink->buffer_sink,
		BUFFERSINK_PARAM_SAMPLE_RATES,
		out_sample_rates,
		-1,
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_int_list(sample rates) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// init the encoder
	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	sink->encoder = encoder;

	encoder->sample_fmt = ENCODER_INPUT_SAMPLE_FORMAT;
	encoder->sample_rate = reference_track->media_info.u.audio.sample_rate;
	encoder->channel_layout = channel_layout;
	encoder->channels = reference_track->media_info.u.audio.channels;
	encoder->bit_rate = reference_track->media_info.bitrate;
	encoder->flags |= CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// add to the inputs list
	input_link = avfilter_inout_alloc();
	if (input_link == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avfilter_inout_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	input_link->filter_ctx = sink->buffer_sink;
	input_link->pad_idx = 0;
	input_link->next = *inputs;
	*inputs = input_link;

	input_link->name = av_strdup((const char*)sink_name);
	if (input_link->name == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: av_strdup failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

static vod_status_t 
audio_filter_init_sources_and_graph_desc(audio_filter_init_context_t* state, media_clip_t* clip)
{
	audio_filter_source_t* cur_source;
	frame_list_part_t* part;
	media_clip_t** sources_end;
	media_clip_t** sources_cur;
	media_clip_source_t* source;
	media_track_t* audio_track;
	media_track_t* cur_track;
	u_char filter_name[VOD_INT32_LEN + 1];
	vod_status_t rc;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;

	if (clip->type == MEDIA_CLIP_SOURCE)
	{
		source = vod_container_of(clip, media_clip_source_t, base);

		// find the audio track
		audio_track = NULL;
		for (cur_track = source->track_array.first_track; cur_track < source->track_array.last_track; cur_track++)
		{
			if (cur_track->media_info.media_type == MEDIA_TYPE_AUDIO)
			{
				audio_track = cur_track;
				break;
			}
		}

		// update the max frame size
		part = &audio_track->frames;
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

		// initialize the source
		cur_source = state->cur_source;
		state->cur_source++;

		cur_source->cur_frame_part = audio_track->frames;
		cur_source->cur_frame = audio_track->frames.first_frame;
		cur_source->dts = 0;

		cur_source->cur_frame_part.frames_source->set_cache_slot_id(
			cur_source->cur_frame_part.frames_source_context,
			state->cache_slot_id++);

		vod_sprintf(filter_name, "%uD%Z", clip->id);

		return audio_filter_init_source(
			state->request_context,
			state->filter_graph,
			filter_name,
			&audio_track->media_info,
			cur_source,
			state->outputs);
	}

	// recursively init the child sources
	sources_end = clip->sources + clip->source_count;
	for (sources_cur = clip->sources; sources_cur < sources_end; sources_cur++)
	{
		if (*sources_cur == NULL)
		{
			continue;
		}

		rc = audio_filter_init_sources_and_graph_desc(state, *sources_cur);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// add the filter description
	if (state->graph_desc_pos > state->graph_desc)
	{
		*state->graph_desc_pos++ = ';';
	}

	state->graph_desc_pos = clip->audio_filter->append_filter_desc(state->graph_desc_pos, clip);

	return VOD_OK;
}

vod_status_t
audio_filter_alloc_state(
	request_context_t* request_context,
	media_sequence_t* sequence,
	media_clip_t* clip,
	media_track_t* output_track,
	size_t* cache_buffer_count,
	void** result)
{
	audio_filter_init_context_t init_context;
	u_char filter_name[VOD_INT32_LEN + 1];
	audio_filter_state_t* state;
	vod_pool_cleanup_t *cln;
	AVFilterInOut *outputs = NULL;
	AVFilterInOut *inputs = NULL;
	uint32_t initial_alloc_size;
	vod_status_t rc;
	int avrc;

	// get the source count and graph desc size
	init_context.request_context = request_context;
	init_context.graph_desc_size = 0;
	init_context.source_count = 0;
	init_context.output_frame_count = 0;

	rc = audio_filter_walk_filters_prepare_init(&init_context, &clip, 100, 100);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (clip == NULL || init_context.source_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: unexpected - no sources found");
		return VOD_UNEXPECTED;
	}

	if (clip->type == MEDIA_CLIP_SOURCE)
	{
		// got left with a source, following a mix of a single source, nothing to do
		return VOD_OK;
	}

	if (!initialized)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	if (init_context.output_frame_count > MAX_FRAME_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: expected output frame count %uD too big", init_context.output_frame_count);
		return VOD_BAD_REQUEST;
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
	
	// add to the cleanup pool
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = audio_filter_free_state;
	cln->data = state;

	// allocate the filter graph
	state->filter_graph = avfilter_graph_alloc();
	if (state->filter_graph == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_alloc_state: avfilter_graph_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// allocate the graph desc and sources
	init_context.graph_desc = vod_alloc(request_context->pool, init_context.graph_desc_size + 
		sizeof(state->sources[0]) * init_context.source_count);
	if (init_context.graph_desc == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->sources = (void*)(init_context.graph_desc + init_context.graph_desc_size);
	state->sources_end = state->sources + init_context.source_count;
	vod_memzero(state->sources, (u_char*)state->sources_end - (u_char*)state->sources);

	// initialize the sources and the graph description
	init_context.filter_graph = state->filter_graph;
	init_context.outputs = &outputs;
	init_context.cur_source = state->sources;
	init_context.graph_desc_pos = init_context.graph_desc;
	init_context.max_frame_size = 0;
	init_context.cache_slot_id = 0;

	rc = audio_filter_init_sources_and_graph_desc(&init_context, clip);
	if (rc != VOD_OK)
	{
		goto end;
	}

	*init_context.graph_desc_pos = '\0';

	// initialize the sink
	vod_sprintf(filter_name, "%uD%Z", clip->id);

	rc = audio_filter_init_sink(
		request_context,
		state->filter_graph,
		output_track,
		filter_name,
		&state->sink,
		&inputs);
	if (rc != VOD_OK)
	{
		goto end;
	}

	// parse the graph description
	avrc = avfilter_graph_parse_ptr(state->filter_graph, (char*)init_context.graph_desc, &inputs, &outputs, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avfilter_graph_parse_ptr failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	// validate and configure the graph
	avrc = avfilter_graph_config(state->filter_graph, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avfilter_graph_config failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	// set the buffer sink frame size
	if ((state->sink.encoder->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE) == 0)
	{
		av_buffersink_set_frame_size(state->sink.buffer_sink, state->sink.encoder->frame_size);
	}
	
	// allocate frames
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: av_frame_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}
	state->filtered_frame = av_frame_alloc();
	if (state->filtered_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: av_frame_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	// initialize the output arrays
	initial_alloc_size = init_context.output_frame_count + 10;

	if (vod_array_init(&state->frames_array, request_context->pool, initial_alloc_size, sizeof(input_frame_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_array_init failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->sequence = sequence;
	state->output = output_track;
	state->max_frame_size = init_context.max_frame_size;
	state->cur_frame_pos = 0;
	state->first_time = TRUE;
	state->cur_source = NULL;
	state->frame_buffer = NULL;

	*cache_buffer_count = init_context.cache_slot_id;
	*result = state;

end:

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return rc;
}

void
audio_filter_free_state(void* context)
{
	audio_filter_state_t* state = (audio_filter_state_t*)context;
	audio_filter_source_t* sources_cur;

	for (sources_cur = state->sources; sources_cur < state->sources_end; sources_cur++)
	{
		avcodec_close(sources_cur->decoder);
		av_free(sources_cur->decoder);
	}
	avcodec_close(state->sink.encoder);
	av_free(state->sink.encoder);
	avfilter_graph_free(&state->filter_graph);
	av_frame_free(&state->filtered_frame);
	av_frame_free(&state->decoded_frame);

	vod_memzero(state, sizeof(*state));		// support calling free twice
}

static vod_status_t 
audio_filter_update_track(audio_filter_state_t* state)
{
	media_track_t* output = state->output;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t old_timescale;
	vod_status_t rc;
	u_char* new_extra_data;
	bool_t has_frames;

	if (state->sink.encoder->time_base.num != 1)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_update_track: unexpected encoder time base %d/%d", 
			state->sink.encoder->time_base.num, state->sink.encoder->time_base.den);
		return VOD_UNEXPECTED;
	}

	// decrement the old frame count and size
	state->sequence->total_frame_count -= output->frame_count;
	state->sequence->total_frame_size -= output->total_frames_size;
	output->total_frames_size = 0;
	output->total_frames_duration = 0;

	// update frames
	output->frame_count = state->frames_array.nelts;

	output->frames.first_frame = state->frames_array.elts;
	output->frames.last_frame = output->frames.first_frame + output->frame_count;
	output->frames.next = NULL;

	// check whether there are any frames with duration
	has_frames = FALSE;
	
	// Note: always a single part here
	last_frame = output->frames.last_frame;
	for (cur_frame = output->frames.first_frame; cur_frame < last_frame; cur_frame++)
	{
		if (cur_frame->duration != 0)
		{
			has_frames = TRUE;
			break;
		}
	}

	if (!has_frames)
	{
		output->frames.first_frame = NULL;
		output->frames.last_frame = NULL;
		output->frame_count = 0;
		return VOD_OK;
	}
	
	// update the frames source to memory
	rc = frames_source_memory_init(state->request_context, &output->frames.frames_source_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	output->frames.frames_source = &frames_source_memory;

	// calculate the total frames size and duration
	for (cur_frame = output->frames.first_frame; cur_frame < last_frame; cur_frame++)
	{
		output->total_frames_size += cur_frame->size;
		output->total_frames_duration += cur_frame->duration;
	}
	
	// update media info
	old_timescale = output->media_info.timescale;
	output->media_info.timescale = state->sink.encoder->time_base.den;
	output->media_info.duration = rescale_time(output->media_info.duration, old_timescale, output->media_info.timescale);
	output->media_info.bitrate = state->sink.encoder->bit_rate;
	
	output->media_info.u.audio.object_type_id = 0x40;		// ffmpeg always writes 0x40 (ff_mp4_obj_type)
	output->media_info.u.audio.channels = state->sink.encoder->channels;
	output->media_info.u.audio.bits_per_sample = ENCODER_BITS_PER_SAMPLE;
	output->media_info.u.audio.packet_size = 0;				// ffmpeg always writes 0 (mov_write_audio_tag)
	output->media_info.u.audio.sample_rate = state->sink.encoder->sample_rate;
	
	output->key_frame_count = 0;
	output->first_frame_time_offset = rescale_time(output->first_frame_time_offset, old_timescale, output->media_info.timescale);
	
	new_extra_data = vod_alloc(state->request_context->pool, state->sink.encoder->extradata_size);
	if (new_extra_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_filter_update_track: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(new_extra_data, state->sink.encoder->extradata, state->sink.encoder->extradata_size);
	
	output->media_info.extra_data.data = new_extra_data;
	output->media_info.extra_data.len = state->sink.encoder->extradata_size;

	if (output->media_info.codec_name.data != NULL)
	{
		rc = codec_config_get_audio_codec_name(state->request_context, &output->media_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
		
	// add the new frame count and size
	state->sequence->total_frame_count += output->frame_count;
	state->sequence->total_frame_size += output->total_frames_size;

	// TODO: update raw_atoms
	
	return VOD_OK;
}

static vod_status_t
audio_filter_write_frame(audio_filter_state_t* state, AVPacket* output_packet)
{
	input_frame_t* cur_frame;
	u_char* new_data;

	cur_frame = vod_array_push(&state->frames_array);
	if (cur_frame == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_filter_write_frame: vod_array_push failed");
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

	cur_frame->duration = output_packet->duration;
	cur_frame->size = output_packet->size;
	cur_frame->key_frame = 0;
	cur_frame->pts_delay = output_packet->pts - output_packet->dts;
	cur_frame->offset = (uintptr_t)new_data;

	return VOD_OK;
}

static vod_status_t
audio_filter_flush_encoder(audio_filter_state_t* state)
{
	AVPacket output_packet;
	vod_status_t rc;
	int avrc;

	avrc = avcodec_send_frame(state->sink.encoder, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_flush_encoder: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	for (;;)
	{
		av_init_packet(&output_packet);
		output_packet.data = NULL; // packet data will be allocated by the encoder
		output_packet.size = 0;
		
		avrc = avcodec_receive_packet(state->sink.encoder, &output_packet);
		if (avrc == AVERROR_EOF)
		{
			break;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_flush_encoder: avcodec_receive_packet failed %d", avrc);
			return VOD_UNEXPECTED;
		}

		rc = audio_filter_write_frame(state, &output_packet);

		av_packet_unref(&output_packet);

		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	return audio_filter_update_track(state);
}

#ifdef AUDIO_FILTER_DEBUG
static void
audio_filter_append_debug_data(const char* source, const char* extension, const void* buffer, size_t size)
{
	char file_path[256];
	FILE* fp;

	sprintf(file_path, "%s%s.%s", AUDIO_FILTER_DEBUG_PATH, source, extension);
		
	fp = fopen(file_path, "a");
	if (fp == NULL)
	{
		return;
	}
	fwrite(buffer, 1, size, fp);
	fclose(fp);
}
#endif // AUDIO_FILTER_DEBUG

static vod_status_t
audio_filter_read_filter_sink(audio_filter_state_t* state)
{
	AVPacket output_packet;
	vod_status_t rc;
	int avrc;
#ifdef AUDIO_FILTER_DEBUG
	size_t data_size;
#endif // AUDIO_FILTER_DEBUG

	for (;;)
	{
		avrc = av_buffersink_get_frame_flags(state->sink.buffer_sink, state->filtered_frame, AV_BUFFERSINK_FLAG_NO_REQUEST);
		if (avrc == AVERROR(EAGAIN) || avrc == AVERROR_EOF)
		{
			break;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_read_filter_sink: av_buffersink_get_frame_flags failed %d", avrc);
			return VOD_UNEXPECTED;
		}

#ifdef AUDIO_FILTER_DEBUG
		data_size = av_samples_get_buffer_size(
			NULL,
			state->sink.encoder->channels,
			state->filtered_frame->nb_samples,
			state->sink.encoder->sample_fmt,
			1);
		audio_filter_append_debug_data("sink", "pcm", state->filtered_frame->data[0], data_size);
#endif // AUDIO_FILTER_DEBUG

		av_init_packet(&output_packet);
		output_packet.data = NULL; // packet data will be allocated by the encoder
		output_packet.size = 0;

		avrc = avcodec_send_frame(state->sink.encoder, state->filtered_frame);

		av_frame_unref(state->filtered_frame);

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_read_filter_sink: avcodec_send_frame failed %d", avrc);
			return VOD_UNEXPECTED;
		}

		avrc = avcodec_receive_packet(state->sink.encoder, &output_packet);

		if (avrc == AVERROR(EAGAIN))
		{
			continue;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_read_filter_sink: avcodec_receive_packet failed %d", avrc);
			return VOD_ALLOC_FAILED;
		}

		rc = audio_filter_write_frame(state, &output_packet);

		av_packet_unref(&output_packet);

		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t 
audio_filter_process_frame(audio_filter_state_t* state, u_char* buffer)
{
	audio_filter_source_t* source = state->cur_source;
	input_frame_t* frame = source->cur_frame;
	AVPacket input_packet;
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int avrc;
#ifdef AUDIO_FILTER_DEBUG
	size_t data_size;
#endif // AUDIO_FILTER_DEBUG
	
#ifdef AUDIO_FILTER_DEBUG
	audio_filter_append_debug_data("input", "aac", buffer, frame->size);
#endif // AUDIO_FILTER_DEBUG
	
	vod_memzero(&input_packet, sizeof(input_packet));
	input_packet.data = buffer;
	input_packet.size = frame->size;
	input_packet.dts = source->dts;
	input_packet.pts = source->dts + frame->pts_delay;
	input_packet.duration = frame->duration;
	input_packet.flags = AV_PKT_FLAG_KEY;
	source->dts += frame->duration;
	
	av_frame_unref(state->decoded_frame);

	frame_end = buffer + frame->size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));

	avrc = avcodec_send_packet(source->decoder, &input_packet);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_process_frame: avcodec_send_packet failed %d", avrc);
		return VOD_BAD_DATA;
	}

	avrc = avcodec_receive_frame(source->decoder, state->decoded_frame);

	vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	if (avrc == AVERROR(EAGAIN))
	{
		return VOD_OK;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_process_frame: avcodec_receive_frame failed %d", avrc);
		return VOD_BAD_DATA;
	}

#ifdef AUDIO_FILTER_DEBUG
	data_size = av_samples_get_buffer_size(
		NULL, 
		source->decoder->channels,
		state->decoded_frame->nb_samples,
		source->decoder->sample_fmt,
		1);
	audio_filter_append_debug_data(source->buffer_src->name, "pcm", state->decoded_frame->data[0], data_size);
#endif // AUDIO_FILTER_DEBUG
	
	avrc = av_buffersrc_add_frame_flags(source->buffer_src, state->decoded_frame, AV_BUFFERSRC_FLAG_PUSH);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_process_frame: av_buffersrc_add_frame_flags failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	return audio_filter_read_filter_sink(state);
}

static vod_status_t
audio_filter_choose_source(audio_filter_state_t* state, audio_filter_source_t** result)
{
	audio_filter_source_t* sources_cur;
	audio_filter_source_t* best_source;
	int failed_requests_max;
	int failed_requests;
	int ret;
	vod_status_t rc;

	for (;;)
	{
		ret = avfilter_graph_request_oldest(state->filter_graph);
		if (ret < 0)
		{
			break;
		}

		rc = audio_filter_read_filter_sink(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	if (ret != AVERROR(EAGAIN))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_choose_source: avfilter_graph_request_oldest failed %d", ret);
		return VOD_UNEXPECTED;
	}

	failed_requests_max = -1;
	best_source = NULL;
	for (sources_cur = state->sources; sources_cur < state->sources_end; sources_cur++)
	{
		if (sources_cur->cur_frame >= sources_cur->cur_frame_part.last_frame)
		{
			if (sources_cur->cur_frame_part.next == NULL)
			{
				continue;
			}

			sources_cur->cur_frame_part = *sources_cur->cur_frame_part.next;
			sources_cur->cur_frame = sources_cur->cur_frame_part.first_frame;
		}

		failed_requests = av_buffersrc_get_nb_failed_requests(sources_cur->buffer_src);
		if (failed_requests > failed_requests_max)
		{
			failed_requests_max = failed_requests;
			best_source = sources_cur;
		}
	}

	*result = best_source;

	return VOD_OK;
}

vod_status_t
audio_filter_process(void* context)
{
	audio_filter_state_t* state = context;
	audio_filter_source_t* source;
	u_char* read_buffer;
	uint32_t read_size;
	bool_t processed_data = FALSE;
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		// choose a source if needed
		if (state->cur_source == NULL)
		{
			rc = audio_filter_choose_source(state, &source);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (source == NULL)
			{
				// done
				return audio_filter_flush_encoder(state);
			}

			state->cur_source = source;

			// start the frame
			rc = source->cur_frame_part.frames_source->start_frame(
				source->cur_frame_part.frames_source_context, 
				source->cur_frame,
				ULLONG_MAX);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		else
		{
			source = state->cur_source;
		}

		// read some data from the frame
		rc = source->cur_frame_part.frames_source->read(
			source->cur_frame_part.frames_source_context,
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
					"audio_filter_process: no data was handled, probably a truncated file");
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
						"audio_filter_process: vod_alloc failed");
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
		rc = audio_filter_process_frame(state, read_buffer);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// move to the next frame
		source->cur_frame++;
		state->cur_source = NULL;
	}
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
	media_sequence_t* sequence,
	media_clip_t* clip,
	media_track_t* output_track,
	size_t* cache_buffer_count,
	void** result)
{
	audio_filter_init_context_t init_context;
	vod_status_t rc;

	// get the source count and graph desc size
	init_context.request_context = request_context;
	init_context.graph_desc_size = 0;
	init_context.source_count = 0;
	init_context.output_frame_count = 0;

	rc = audio_filter_walk_filters_prepare_init(&init_context, &clip, 100, 100);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (clip == NULL || init_context.source_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: unexpected - no sources found");
		return VOD_UNEXPECTED;
	}

	if (clip->type == MEDIA_CLIP_SOURCE)
	{
		// got left with a source, following a mix of a single source, nothing to do
		return VOD_OK;
	}

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"audio_filter_alloc_state: audio filtering not supported, recompile with avcodec/avfilter to enable it");
	return VOD_UNEXPECTED;
}

void 
audio_filter_free_state(void* context)
{
}

vod_status_t 
audio_filter_process(void* context)
{
	return VOD_UNEXPECTED;
}

#endif
