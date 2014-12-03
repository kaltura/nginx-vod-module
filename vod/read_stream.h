#ifndef __READ_STREAM_H__
#define __READ_STREAM_H__

// includes
#include "common.h"

// int parsing macros
#define PARSE_LE32(p) ( ((uint32_t) ((u_char*)p)[3] << 24) | (((u_char*)p)[2] << 16) | (((u_char*)p)[1] << 8) | (((u_char*)p)[0]) )
#define PARSE_BE16(p) ( ((uint16_t) ((u_char*)p)[0] << 8)  | (((u_char*)p)[1]) )
#define PARSE_BE32(p) ( ((uint32_t) ((u_char*)p)[0] << 24) | (((u_char*)p)[1] << 16) | (((u_char*)p)[2] << 8) | (((u_char*)p)[3]) )
#define PARSE_BE64(p) ((((uint64_t)PARSE_BE32(p)) << 32) | PARSE_BE32((p) + 4))

// int reading macros
#define READ_LE32(p, v) { v = PARSE_LE32(p); p += sizeof(uint32_t); }
#define READ_BE16(p, v) { v = PARSE_BE16(p); p += sizeof(uint16_t); }
#define READ_BE32(p, v) { v = PARSE_BE32(p); p += sizeof(uint32_t); }
#define READ_BE64(p, v) { v = PARSE_BE64(p); p += sizeof(uint64_t); }

// typedefs
typedef struct {
	const u_char* cur_pos;
	const u_char* end_pos;
	bool_t eof_reached;
} simple_read_stream_t;

// functions
static vod_inline u_char 
read_stream_get_byte(simple_read_stream_t* stream)
{
	if (stream->cur_pos >= stream->end_pos)
	{
		stream->eof_reached = TRUE;
		return 0;
	}
	return *stream->cur_pos++;
}

static vod_inline uint32_t
read_stream_get_be32(simple_read_stream_t* stream)
{
	uint32_t result;

	if (stream->cur_pos + sizeof(uint32_t) > stream->end_pos)
	{
		stream->eof_reached = TRUE;
		stream->cur_pos = stream->end_pos;
		return 0;
	}

	READ_BE32(stream->cur_pos, result);
	return result;
}

static vod_inline void 
read_stream_skip(simple_read_stream_t* stream, int bytes)
{
	if (stream->cur_pos + bytes > stream->end_pos)
	{
		stream->eof_reached = TRUE;
		stream->cur_pos = stream->end_pos;
	}
	else
	{
		stream->cur_pos += bytes;
	}
}

#endif // __READ_STREAM_H__
