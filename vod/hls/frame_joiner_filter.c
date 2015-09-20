#include "frame_joiner_filter.h"
#include "mpegts_encoder_filter.h"

#define NO_TIMESTAMP ((uint64_t)-1)
#define FRAME_OUTPUT_INTERVAL (63000)	// 0.7 sec

void 
frame_joiner_init(
	frame_joiner_t* state, 
	const media_filter_t* next_filter,
	void* next_filter_context)
{
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;

	state->frame_dts = NO_TIMESTAMP;
}

static vod_status_t
frame_joiner_start_frame(void* context, output_frame_t* frame)
{
	frame_joiner_t* state = (frame_joiner_t*)context;
	vod_status_t rc;

	if (frame->dts >= state->frame_dts + FRAME_OUTPUT_INTERVAL && state->frame_dts != NO_TIMESTAMP)
	{
		rc = state->next_filter->flush_frame(state->next_filter_context, FALSE);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->frame_dts = NO_TIMESTAMP;
	}

	if (state->frame_dts == NO_TIMESTAMP)
	{
		rc = state->next_filter->start_frame(state->next_filter_context, frame);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->frame_dts = frame->dts;
	}
	else
	{
		rc = mpegts_encoder_start_sub_frame(state->next_filter_context, frame);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t
frame_joiner_write(void* context, const u_char* buffer, uint32_t size)
{
	frame_joiner_t* state = (frame_joiner_t*)context;

	return state->next_filter->write(state->next_filter_context, buffer, size);
}

static vod_status_t
frame_joiner_flush_frame(void* context, bool_t last_stream_frame)
{
	frame_joiner_t* state = (frame_joiner_t*)context;

	if (!last_stream_frame)
	{
		return VOD_OK;
	}

	state->frame_dts = NO_TIMESTAMP;

	return state->next_filter->flush_frame(state->next_filter_context, TRUE);
}


static void
frame_joiner_simulated_start_frame(void* context, output_frame_t* frame)
{
	frame_joiner_t* state = (frame_joiner_t*)context;

	if (frame->dts >= state->frame_dts + FRAME_OUTPUT_INTERVAL && state->frame_dts != NO_TIMESTAMP)
	{
		state->next_filter->simulated_flush_frame(state->next_filter_context, FALSE);

		state->frame_dts = NO_TIMESTAMP;
	}

	if (state->frame_dts == NO_TIMESTAMP)
	{
		state->next_filter->simulated_start_frame(state->next_filter_context, frame);

		state->frame_dts = frame->dts;
	}
}

static void
frame_joiner_simulated_write(void* context, uint32_t size)
{
	frame_joiner_t* state = (frame_joiner_t*)context;

	state->next_filter->simulated_write(state->next_filter_context, size);
}

static void
frame_joiner_simulated_flush_frame(void* context, bool_t last_stream_frame)
{
	frame_joiner_t* state = (frame_joiner_t*)context;

	if (last_stream_frame)
	{
		state->frame_dts = NO_TIMESTAMP;

		state->next_filter->simulated_flush_frame(state->next_filter_context, TRUE);
	}
}


const media_filter_t frame_joiner = {
	frame_joiner_start_frame,
	frame_joiner_write,
	frame_joiner_flush_frame,
	frame_joiner_simulated_start_frame,
	frame_joiner_simulated_write,
	frame_joiner_simulated_flush_frame,
};
