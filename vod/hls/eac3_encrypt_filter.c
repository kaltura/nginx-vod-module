#include "eac3_encrypt_filter.h"
#include "frame_encrypt_filter.h"
#include "../read_stream.h"

// macros
#define THIS_FILTER (MEDIA_FILTER_EAC3_ENCRYPT)
#define get_context(ctx) ((eac3_encrypt_filter_state_t*)ctx->context[THIS_FILTER])

#define AC3_HEADER_SIZE (7)

// typedefs
typedef struct
{
	// fixed input data
	media_filter_start_frame_t start_frame;
	media_filter_write_t write;

	// state
	u_char header[AC3_HEADER_SIZE];
	uint32_t header_left;
	uint32_t frame_size_left;
	uint32_t sync_frame_size_left;
} eac3_encrypt_filter_state_t;

static vod_status_t
eac3_encrypt_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	eac3_encrypt_filter_state_t* state = get_context(context);

	if (frame->size < sizeof(state->header))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"eac3_encrypt_start_frame: frame size %uD too small", frame->size);
		return VOD_BAD_DATA;
	}

	state->frame_size_left = frame->size;
	state->header_left = sizeof(state->header);

	return state->start_frame(context, frame);
}

static vod_status_t
eac3_encrypt_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	eac3_encrypt_filter_state_t* state = get_context(context);
	uint32_t sync_frame_size;
	uint32_t cur_size;
	vod_status_t rc;

	for (;;)
	{
		if (state->header_left > 0)
		{
			// copy as much as possible to the sync frame header
			cur_size = vod_min(state->header_left, size);
			vod_memcpy(state->header + sizeof(state->header) - state->header_left, buffer, cur_size);
			state->header_left -= cur_size;
			if (state->header_left > 0)
			{
				return VOD_OK;
			}

			// get the sync frame size
			if (state->header[0] != 0x0b || state->header[1] != 0x77)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"eac3_encrypt_write: invalid sync frame magic 0x%02uxD 0x%02uxD", 
					(uint32_t)state->header[0], (uint32_t)state->header[1]);
				return VOD_BAD_DATA;
			}

			sync_frame_size = parse_be16(state->header + 2) & 0x7ff;
			sync_frame_size = (sync_frame_size + 1) << 1;

			if (sync_frame_size < sizeof(state->header) ||
				sync_frame_size > state->frame_size_left)
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"eac3_encrypt_write: invalid sync frame size %uD", sync_frame_size);
				return VOD_BAD_DATA;
			}

			state->frame_size_left -= sync_frame_size;
			if (state->frame_size_left > 0 &&
				state->frame_size_left < sizeof(state->header))
			{
				vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
					"eac3_encrypt_write: invalid frame size left %uD", state->frame_size_left);
				return VOD_BAD_DATA;
			}

			// start the sync frame
			frame_encrypt_start_sub_frame(context, sync_frame_size);

			rc = state->write(context, state->header, sizeof(state->header));
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->sync_frame_size_left = sync_frame_size - sizeof(state->header);

			buffer += cur_size;
			size -= cur_size;
		}

		// write as much as possible
		cur_size = vod_min(state->sync_frame_size_left, size);
		rc = state->write(context, buffer, cur_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->sync_frame_size_left -= cur_size;
		if (state->sync_frame_size_left > 0)
		{
			return VOD_OK;
		}

		state->header_left = sizeof(state->header);
		size -= cur_size;
		if (size <= 0)
		{
			break;
		}

		buffer += cur_size;
	}

	return VOD_OK;
}

vod_status_t
eac3_encrypt_filter_init(
	media_filter_t* filter,
	media_filter_context_t* context)
{
	eac3_encrypt_filter_state_t* state;
	request_context_t* request_context = context->request_context;

	// allocate state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"eac3_encrypt_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// save required functions
	state->start_frame = filter->start_frame;
	state->write = filter->write;

	// override functions
	filter->start_frame = eac3_encrypt_start_frame;
	filter->write = eac3_encrypt_write;

	// save the context
	context->context[THIS_FILTER] = state;

	return VOD_OK;
}
