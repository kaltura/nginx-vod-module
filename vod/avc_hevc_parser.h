#ifndef __AVC_HEVC_PARSER_H__
#define __AVC_HEVC_PARSER_H__

#include "bit_read_stream.h"

// constants
#define MAX_SPS_COUNT (32)
#define MAX_PPS_COUNT (256)
#define EXTENDED_SAR (255)

// macros
#define bit_read_stream_skip_signed_exp bit_read_stream_skip_unsigned_exp

// typedefs
typedef struct {
	request_context_t* request_context;
	vod_array_t sps;
	vod_array_t pps;
} avc_hevc_parse_ctx_t;

// bit stream inlines
static vod_inline void
bit_read_stream_skip_unsigned_exp(bit_reader_state_t* reader)
{
	int zero_count;

	for (zero_count = 0; bit_read_stream_get_one(reader) == 0 && !reader->stream.eof_reached; zero_count++);

	bit_read_stream_skip(reader, zero_count);
}

static vod_inline uint32_t
bit_read_stream_get_unsigned_exp(bit_reader_state_t* reader)
{
	int zero_count;

	for (zero_count = 0; bit_read_stream_get_one(reader) == 0 && !reader->stream.eof_reached; zero_count++);

	return (1 << zero_count) - 1 + bit_read_stream_get(reader, zero_count);
}

static vod_inline int32_t
bit_read_stream_get_signed_exp(bit_reader_state_t* reader)
{
	int32_t value = bit_read_stream_get_unsigned_exp(reader);
	if (value > 0)
	{
		if (value & 1)		// positive
		{
			value = (value + 1) / 2;
		}
		else
		{
			value = -(value / 2);
		}
	}
	return value;
}

// functions
bool_t avc_hevc_parser_rbsp_trailing_bits(bit_reader_state_t* reader);

void* avc_hevc_parser_get_ptr_array_item(vod_array_t* arr, size_t index, size_t size);

uint32_t avc_hevc_parser_emulation_prevention_encode_bytes(
	const u_char* cur_pos,
	const u_char* end_pos);

vod_status_t avc_hevc_parser_emulation_prevention_decode(
	request_context_t* request_context,
	bit_reader_state_t* reader,
	const u_char* buffer,
	uint32_t size);

unsigned avc_hevc_parser_ceil_log2(unsigned val);

vod_status_t avc_hevc_parser_init_ctx(
	request_context_t* request_context,
	void** result);

#endif //__AVC_HEVC_PARSER_H__
