#include "buffer_filter.h"
#include "mpegts_encoder_filter.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_BUFFER)
#define get_context(ctx) ((buffer_filter_t*)ctx->context[THIS_FILTER])

#define BUFFERED_FRAMES_QUEUE_SIZE (28)		// 28 = ceil(188/7) + 1, 188 = ts packet size, 7 = min audio frame size (adts header)

// typedefs
typedef struct {
	output_frame_t frame;
	u_char* end_pos;
} buffered_frame_info_t;

typedef struct {
	buffered_frame_info_t data[BUFFERED_FRAMES_QUEUE_SIZE];
	uint32_t write_pos;
	uint32_t read_pos;
	bool_t is_full;
} buffered_frames_queue_t;

typedef struct {
	// input data
	media_filter_t next_filter;
	bool_t align_frames;
	uint32_t size;

	// fixed
	u_char* start_pos;
	u_char* end_pos;

	// state
	int cur_state;
	output_frame_t cur_frame;
	output_frame_t last_frame;
	u_char* cur_pos;
	u_char* last_flush_pos;

	buffered_frames_queue_t buffered_frames;

	// simulation mode
	uint32_t used_size;
	uint32_t last_flush_size;
} buffer_filter_t;

enum {
	STATE_INITIAL,
	STATE_FRAME_STARTED,
	STATE_FRAME_FLUSHED,
	STATE_DIRECT,
};

static vod_status_t 
buffer_filter_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	buffer_filter_t* state = get_context(context);
	
	switch (state->cur_state)
	{
	case STATE_INITIAL:
		state->cur_frame = *frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		break;
		
	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"buffer_filter_start_frame: invalid state %d", state->cur_state);
		return VOD_UNEXPECTED;
	}

	state->last_frame = *frame;
	state->cur_state = STATE_FRAME_STARTED;

	return VOD_OK;
}

vod_status_t 
buffer_filter_force_flush(media_filter_context_t* context, bool_t last_stream_frame)
{
	buffer_filter_t* state = get_context(context);
	vod_status_t rc;
	u_char* np;
	u_char* p;

	// if nothing was written since the last frame flush, nothing to do
	if (state->last_flush_pos <= state->start_pos)
	{
		return VOD_OK;
	}
	
	// Note: at this point state can only be either STATE_FRAME_STARTED or STATE_FRAME_FLUSHED
		
	// write all buffered data up to the last frame flush position
	rc = state->next_filter.start_frame(context, &state->cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (!state->align_frames)
	{
		if (state->buffered_frames.is_full)
		{
			state->buffered_frames.is_full = FALSE;

			// queue is full, make the loop below start from the oldest frame
			state->buffered_frames.read_pos = state->buffered_frames.write_pos + 1;
			if (state->buffered_frames.read_pos >= BUFFERED_FRAMES_QUEUE_SIZE)
			{
				state->buffered_frames.read_pos = 0;
			}
		}

		// when the frames are not aligned, need to write each frame separately since the pts
		// that is outputted may be the pts of some previous frame
		p = state->start_pos;

		while (state->buffered_frames.write_pos != state->buffered_frames.read_pos)
		{
			if (p > state->start_pos)
			{
				rc = mpegts_encoder_start_sub_frame(context, &state->buffered_frames.data[state->buffered_frames.read_pos].frame);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

			np = state->buffered_frames.data[state->buffered_frames.read_pos].end_pos;

			rc = state->next_filter.write(context, p, np - p);
			if (rc != VOD_OK)
			{
				return rc;
			}

			p = np;

			state->buffered_frames.read_pos++;
			if (state->buffered_frames.read_pos >= BUFFERED_FRAMES_QUEUE_SIZE)
			{
				state->buffered_frames.read_pos = 0;
			}
		}
	}
	else
	{
		rc = state->next_filter.write(context, state->start_pos, state->last_flush_pos - state->start_pos);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	rc = state->next_filter.flush_frame(context, last_stream_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	// move back any remaining data
	vod_memmove(state->start_pos, state->last_flush_pos, state->cur_pos - state->last_flush_pos);
	state->cur_pos -= (state->last_flush_pos - state->start_pos);
	state->last_flush_pos = state->start_pos;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		state->cur_frame = state->last_frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		state->cur_state = STATE_INITIAL;
		break;
	}
	
	return VOD_OK;
}

static vod_status_t 
buffer_filter_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	buffer_filter_t* state = get_context(context);
	vod_status_t rc;

	switch (state->cur_state)
	{
	case STATE_DIRECT:
		// in direct mode just pass the write to the next filter
		return state->next_filter.write(context, buffer, size);
		
	case STATE_FRAME_STARTED:
		break;				// handled outside the switch
		
	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"buffer_filter_write: invalid state %d", state->cur_state);
		return VOD_UNEXPECTED;		// unexpected
	}
	
	// if there is not enough room try flushing the buffer
	if (state->cur_pos + size > state->end_pos)
	{
		rc = buffer_filter_force_flush(context, FALSE);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	// if there is enough room in the buffer, copy the input data
	if (state->cur_pos + size <= state->end_pos)
	{
		state->cur_pos = vod_copy(state->cur_pos, buffer, size);
		return VOD_OK;
	}
	
	// still not enough room after flushing - write directly to the next filter
	state->cur_state = STATE_DIRECT;
	
	rc = state->next_filter.start_frame(context, &state->cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	if (state->cur_pos > state->start_pos)
	{
		rc = state->next_filter.write(context, state->start_pos, state->cur_pos - state->start_pos);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_pos = state->start_pos;
	}
	
	rc = state->next_filter.write(context, buffer, size);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	return VOD_OK;
}

static vod_status_t 
buffer_filter_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	buffer_filter_t* state = get_context(context);
	vod_status_t rc;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:

		if (!state->align_frames)
		{
			// add the frame to the buffered frames queue
			state->buffered_frames.data[state->buffered_frames.write_pos].frame = state->last_frame;
			state->buffered_frames.data[state->buffered_frames.write_pos].end_pos = state->cur_pos;
			state->buffered_frames.write_pos++;
			if (state->buffered_frames.write_pos >= BUFFERED_FRAMES_QUEUE_SIZE)
			{
				state->buffered_frames.write_pos = 0;
			}

			if (state->buffered_frames.write_pos == state->buffered_frames.read_pos)
			{
				state->buffered_frames.is_full = TRUE;
			}
		}

		// update the last flush position
		state->last_flush_pos = state->cur_pos;
		state->cur_state = STATE_FRAME_FLUSHED;

		if (last_stream_frame)
		{
			rc = buffer_filter_force_flush(context, TRUE);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		break;
	
	case STATE_DIRECT:
		// pass the frame flush to the next filter
		rc = state->next_filter.flush_frame(context, last_stream_frame);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_state = STATE_INITIAL;
		break;
		
		// Note: nothing to do for the other states
	}
	
	return VOD_OK;
}

bool_t 
buffer_filter_get_dts(media_filter_context_t* context, uint64_t* dts)
{
	buffer_filter_t* state = get_context(context);

	if (state->cur_state == STATE_INITIAL)
	{
		return FALSE;
	}
	
	*dts = state->cur_frame.dts;
	return TRUE;
}


void
buffer_filter_simulated_force_flush(media_filter_context_t* context, bool_t last_stream_frame)
{
	buffer_filter_t* state = get_context(context);

	if (state->last_flush_size <= 0)
	{
		return;
	}
	
	state->next_filter.simulated_start_frame(context, &state->cur_frame);
	state->next_filter.simulated_write(context, state->last_flush_size);
	state->next_filter.simulated_flush_frame(context, last_stream_frame);
	
	state->used_size -= state->last_flush_size;
	state->last_flush_size = 0;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		state->cur_frame = state->last_frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		state->cur_state = STATE_INITIAL;
		break;
	}
}

static void 
buffer_filter_simulated_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	buffer_filter_t* state = get_context(context);
	
	switch (state->cur_state)
	{
	case STATE_INITIAL:
		state->cur_frame = *frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		break;
	}

	state->last_frame = *frame;
	state->cur_state = STATE_FRAME_STARTED;
}

static void 
buffer_filter_simulated_write(media_filter_context_t* context, uint32_t size)
{
	buffer_filter_t* state = get_context(context);

	switch (state->cur_state)
	{
	case STATE_DIRECT:
		state->next_filter.simulated_write(context, size);
		return;
		
	case STATE_FRAME_STARTED:
		break;				// handled outside the switch
	}
	
	if (state->used_size + size > state->size)
	{
		buffer_filter_simulated_force_flush(context, FALSE);
	}
	
	if (state->used_size + size <= state->size)
	{
		state->used_size += size;
		return;
	}
	
	state->cur_state = STATE_DIRECT;
	
	state->next_filter.simulated_start_frame(context, &state->cur_frame);
	
	state->next_filter.simulated_write(context, state->used_size + size);

	state->used_size = 0;
}

static void 
buffer_filter_simulated_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	buffer_filter_t* state = get_context(context);

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		// update the last flush position
		state->last_flush_size = state->used_size;
		state->cur_state = STATE_FRAME_FLUSHED;

		if (last_stream_frame)
		{
			buffer_filter_simulated_force_flush(context, TRUE);
		}
		break;
	
	case STATE_DIRECT:
		// pass the frame flush to the next filter
		state->next_filter.simulated_flush_frame(context, last_stream_frame);
		state->cur_state = STATE_INITIAL;
		break;
	}
}

static const media_filter_t buffer_filter = {
	buffer_filter_start_frame,
	buffer_filter_write,
	buffer_filter_flush_frame,
	buffer_filter_simulated_start_frame,
	buffer_filter_simulated_write,
	buffer_filter_simulated_flush_frame,
};

vod_status_t
buffer_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context,
	bool_t align_frames,
	uint32_t size)
{
	buffer_filter_t* state;
	request_context_t* request_context = context->request_context;

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"buffer_filter_init: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->align_frames = align_frames;
	state->size = size;

	state->cur_state = STATE_INITIAL;

	state->used_size = 0;
	state->last_flush_size = 0;

	state->next_filter = *filter;
	*filter = buffer_filter;
	context->context[THIS_FILTER] = state;

	if (request_context->simulation_only)
	{
		return VOD_OK;
	}

	state->start_pos = vod_alloc(request_context->pool, size);
	if (state->start_pos == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"buffer_filter_init: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	state->end_pos = state->start_pos + size;
	state->cur_pos = state->start_pos;
	state->last_flush_pos = state->cur_pos;

	state->buffered_frames.read_pos = 0;
	state->buffered_frames.write_pos = 0;
	state->buffered_frames.is_full = FALSE;

	return VOD_OK;
}
