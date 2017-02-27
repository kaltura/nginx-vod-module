#include "frame_joiner_filter.h"
#include "mpegts_encoder_filter.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_JOINER)
#define get_context(ctx) ((frame_joiner_t*)ctx->context[THIS_FILTER])

// constants
#define NO_TIMESTAMP ((uint64_t)-1)
#define FRAME_OUTPUT_INTERVAL (63000)	// 0.7 sec

// typedefs
typedef struct {
	// input data
	media_filter_start_frame_t start_frame;
	media_filter_flush_frame_t flush_frame;
	media_filter_simulated_start_frame_t simulated_start_frame;
	media_filter_simulated_flush_frame_t simulated_flush_frame;

	// state
	uint64_t frame_dts;
} frame_joiner_t;

static vod_status_t
frame_joiner_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	frame_joiner_t* state = get_context(context);
	vod_status_t rc;

	if (frame->dts >= state->frame_dts + FRAME_OUTPUT_INTERVAL && state->frame_dts != NO_TIMESTAMP)
	{
		rc = state->flush_frame(context, FALSE);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->frame_dts = NO_TIMESTAMP;
	}

	if (state->frame_dts == NO_TIMESTAMP)
	{
		rc = state->start_frame(context, frame);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->frame_dts = frame->dts;
	}
	else
	{
		rc = mpegts_encoder_start_sub_frame(context, frame);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t
frame_joiner_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	frame_joiner_t* state = get_context(context);

	if (!last_stream_frame)
	{
		return VOD_OK;
	}

	state->frame_dts = NO_TIMESTAMP;

	return state->flush_frame(context, TRUE);
}


static void
frame_joiner_simulated_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	frame_joiner_t* state = get_context(context);

	if (frame->dts >= state->frame_dts + FRAME_OUTPUT_INTERVAL && state->frame_dts != NO_TIMESTAMP)
	{
		state->simulated_flush_frame(context, FALSE);

		state->frame_dts = NO_TIMESTAMP;
	}

	if (state->frame_dts == NO_TIMESTAMP)
	{
		state->simulated_start_frame(context, frame);

		state->frame_dts = frame->dts;
	}
}

static void
frame_joiner_simulated_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	frame_joiner_t* state = get_context(context);

	if (last_stream_frame)
	{
		state->frame_dts = NO_TIMESTAMP;

		state->simulated_flush_frame(context, TRUE);
	}
}


vod_status_t
frame_joiner_init(
	media_filter_t* filter,
	media_filter_context_t* context)
{
	frame_joiner_t* state;
	request_context_t* request_context = context->request_context;

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"frame_joiner_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->frame_dts = NO_TIMESTAMP;

	// save required functions
	state->start_frame = filter->start_frame;
	state->flush_frame = filter->flush_frame;
	state->simulated_start_frame = filter->simulated_start_frame;
	state->simulated_flush_frame = filter->simulated_flush_frame;

	// override functions
	filter->start_frame = frame_joiner_start_frame;
	filter->flush_frame = frame_joiner_flush_frame;
	filter->simulated_start_frame = frame_joiner_simulated_start_frame;
	filter->simulated_flush_frame = frame_joiner_simulated_flush_frame;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}
