#include "id3_encoder_filter.h"

// macros
#define write_be32_synchsafe(p, dw)	\
	{								\
	*(p)++ = ((dw) >> 21) & 0x7F;	\
	*(p)++ = ((dw) >> 14) & 0x7F;	\
	*(p)++ = ((dw) >> 7) & 0x7F;	\
	*(p)++ = (dw) & 0x7F;			\
	}

// constants
static u_char header_template[] = {
	// id3 header
	0x49, 0x44, 0x33, 0x04,		// file identifier
	0x00,						// version
	0x00,						// flags
	0x00, 0x00, 0x00, 0x00,		// size

	// frame header
	0x54, 0x45, 0x58, 0x54,		// frame id
	0x00, 0x00, 0x00, 0x00,		// size
	0x00, 0x00,					// flags

	// text frame
	0x03,						// encoding	(=utf8, null term)
};

void
id3_encoder_init(
	id3_encoder_state_t* state, 
	const media_filter_t* next_filter,
	void* next_filter_context)
{
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	vod_memcpy(&state->header, header_template, sizeof(state->header));
}

static vod_status_t 
id3_encoder_start_frame(void* context, output_frame_t* frame)
{
	id3_encoder_state_t* state = (id3_encoder_state_t*)context;
	vod_status_t rc;
	uint32_t size = frame->size;
	u_char* p;

	// start the frame
	frame->size = size + sizeof(state->header);
	
	rc = state->next_filter->start_frame(state->next_filter_context, frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	// write the header
	p = state->header.frame_header.size;
	size += sizeof(id3_text_frame_header_t);
	write_be32_synchsafe(p, size);

	p = state->header.file_header.size;
	size += sizeof(id3_frame_header_t);
	write_be32_synchsafe(p, size);

	return state->next_filter->write(state->next_filter_context, (u_char*)&state->header, sizeof(state->header));
}

static vod_status_t 
id3_encoder_write(void* context, const u_char* buffer, uint32_t size)
{
	id3_encoder_state_t* state = (id3_encoder_state_t*)context;

	return state->next_filter->write(state->next_filter_context, buffer, size);
}

static vod_status_t 
id3_encoder_flush_frame(void* context, bool_t last_stream_frame)
{
	id3_encoder_state_t* state = (id3_encoder_state_t*)context;

	return state->next_filter->flush_frame(state->next_filter_context, last_stream_frame);
}


static void 
id3_encoder_simulated_start_frame(void* context, output_frame_t* frame)
{
	id3_encoder_state_t* state = (id3_encoder_state_t*)context;

	state->next_filter->simulated_start_frame(state->next_filter_context, frame);
	state->next_filter->simulated_write(state->next_filter_context, sizeof(state->header));
}

static void 
id3_encoder_simulated_write(void* context, uint32_t size)
{
	id3_encoder_state_t* state = (id3_encoder_state_t*)context;

	state->next_filter->simulated_write(state->next_filter_context, size);
}

static void 
id3_encoder_simulated_flush_frame(void* context, bool_t last_stream_frame)
{
	id3_encoder_state_t* state = (id3_encoder_state_t*)context;

	state->next_filter->simulated_flush_frame(state->next_filter_context, last_stream_frame);
}


const media_filter_t id3_encoder = {
	id3_encoder_start_frame,
	id3_encoder_write,
	id3_encoder_flush_frame,
	id3_encoder_simulated_start_frame,
	id3_encoder_simulated_write,
	id3_encoder_simulated_flush_frame,
};