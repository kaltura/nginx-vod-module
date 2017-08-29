#ifndef __BIT_READ_STREAM_H__
#define __BIT_READ_STREAM_H__

// includes
#include "read_stream.h"

// typedefs
typedef struct {
	simple_read_stream_t stream;
	u_char cur_byte;
	signed char cur_bit;
} bit_reader_state_t;

// functions
static vod_inline void 
bit_read_stream_init(bit_reader_state_t* state, const u_char* buffer, int size)
{
	state->stream.cur_pos = buffer;
	state->stream.end_pos = buffer + size;
	state->stream.eof_reached = FALSE;
	state->cur_byte = 0;
	state->cur_bit = -1;
}

static vod_inline int
bit_read_stream_get_one(bit_reader_state_t* state)
{
	int result;

	if (state->cur_bit < 0)
	{
		state->cur_byte = read_stream_get_byte(&state->stream);
		state->cur_bit = 7;
	}

	result = ((state->cur_byte >> state->cur_bit) & 1);
	state->cur_bit--;

	return result;
}

static vod_inline void
bit_read_stream_skip(bit_reader_state_t* state, int count)
{
	int skip_bytes;

	state->cur_bit -= count - 1;
	if (state->cur_bit < 0)
	{
		skip_bytes = ((-state->cur_bit + 7) >> 3);
		read_stream_skip(&state->stream, skip_bytes - 1);
		state->cur_byte = read_stream_get_byte(&state->stream);
		state->cur_bit += (skip_bytes << 3);
	}
	state->cur_bit--;
}

static vod_inline int 
bit_read_stream_get(bit_reader_state_t* state, int count)
{
	int result = 0;
	
	for (; count; count--)
	{
		if (state->cur_bit < 0)
		{
			state->cur_byte = read_stream_get_byte(&state->stream);
			state->cur_bit = 7;
		}
	
		result = (result << 1) | ((state->cur_byte >> state->cur_bit) & 1);
		state->cur_bit--;
	}
	return result;
}

static vod_inline int64_t
bit_read_stream_get_long(bit_reader_state_t* state, int count)
{
	int64_t result = 0;

	for (; count; count--)
	{
		if (state->cur_bit < 0)
		{
			state->cur_byte = read_stream_get_byte(&state->stream);
			state->cur_bit = 7;
		}

		result = (result << 1) | ((state->cur_byte >> state->cur_bit) & 1);
		state->cur_bit--;
	}
	return result;
}

#endif // __BIT_READ_STREAM_H__
