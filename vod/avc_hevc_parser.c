#include "avc_hevc_parser.h"

bool_t
avc_hevc_parser_rbsp_trailing_bits(bit_reader_state_t* reader)
{
	uint32_t one_bit;

	if (reader->stream.eof_reached)
	{
		return FALSE;
	}

	one_bit = bit_read_stream_get_one(reader);
	if (one_bit != 1)
	{
		return FALSE;
	}

	while (!reader->stream.eof_reached)
	{
		if (bit_read_stream_get_one(reader) != 0)
		{
			return FALSE;
		}
	}

	return TRUE;
}
// base functions
void*
avc_hevc_parser_get_ptr_array_item(vod_array_t* arr, size_t index, size_t size)
{
	size_t alloc_count;
	void** ptr_ptr;
	void* new_ptr;
	void* ptr;

	if (index >= arr->nelts)
	{
		// grow the array
		alloc_count = index + 1 - arr->nelts;
		new_ptr = vod_array_push_n(arr, alloc_count);
		if (new_ptr == NULL)
		{
			return NULL;
		}

		vod_memzero(new_ptr, alloc_count * arr->size);
	}

	// get the item
	ptr_ptr = (void*)((u_char*)arr->elts + index * arr->size);
	ptr = *ptr_ptr;

	if (ptr == NULL)
	{
		// allocate the item
		ptr = vod_alloc(arr->pool, size);
		if (ptr == NULL)
		{
			return NULL;
		}

		*ptr_ptr = ptr;
	}

	vod_memzero(ptr, size);
	return ptr;
}

// emulation prevention
uint32_t
avc_hevc_parser_emulation_prevention_encode_bytes(
	const u_char* cur_pos,
	const u_char* end_pos)
{
	uint32_t result = 0;

	end_pos -= 2;
	for (; cur_pos < end_pos; cur_pos++)
	{
		if (cur_pos[0] == 0 && cur_pos[1] == 0 && cur_pos[2] <= 3)
		{
			result++;
			cur_pos += 2;
		}
	}

	return result;
}

vod_status_t
avc_hevc_parser_emulation_prevention_decode(
	request_context_t* request_context,
	bit_reader_state_t* reader,
	const u_char* buffer,
	uint32_t size)
{
	const u_char* cur_pos;
	const u_char* end_pos = buffer + size;
	const u_char* limit = end_pos - 2;
	u_char* output;
	u_char cur_byte;
	bool_t requires_strip;

	requires_strip = FALSE;
	for (cur_pos = buffer; cur_pos < limit; cur_pos++)
	{
		if (cur_pos[0] == 0 && cur_pos[1] == 0 && cur_pos[2] == 3)
		{
			requires_strip = TRUE;
			break;
		}
	}

	if (!requires_strip)
	{
		bit_read_stream_init(reader, buffer, size);
		return VOD_OK;
	}

	output = vod_alloc(request_context->pool, size);
	if (output == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"avc_hevc_parser_emulation_prevention_decode: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	bit_read_stream_init(reader, output, 0);	// size updated later

	for (cur_pos = buffer; cur_pos < limit; )
	{
		cur_byte = *cur_pos;
		if (cur_byte == 0 && cur_pos[1] == 0 && cur_pos[2] == 3)
		{
			*output++ = 0;
			*output++ = 0;
			cur_pos += 3;
		}
		else
		{
			*output++ = cur_byte;
			cur_pos++;
		}
	}

	while (cur_pos < end_pos)
	{
		*output++ = *cur_pos++;
	}

	reader->stream.end_pos = output;
	return VOD_OK;
}

unsigned
avc_hevc_parser_ceil_log2(unsigned val)
{
	unsigned result = 0;

	val--;
	while (val != 0)
	{
		val >>= 1;
		result++;
	}
	return result;
}

vod_status_t
avc_hevc_parser_init_ctx(
	request_context_t* request_context,
	void** result)
{
	avc_hevc_parse_ctx_t* ctx;

	ctx = vod_alloc(request_context->pool, sizeof(*ctx));
	if (ctx == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"avc_hevc_parser_init_ctx: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	if (vod_array_init(&ctx->sps, request_context->pool, 1, sizeof(void*)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"avc_hevc_parser_init_ctx: vod_array_init failed (1)");
		return VOD_ALLOC_FAILED;
	}

	if (vod_array_init(&ctx->pps, request_context->pool, 1, sizeof(void*)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"avc_hevc_parser_init_ctx: vod_array_init failed (2)");
		return VOD_ALLOC_FAILED;
	}

	ctx->request_context = request_context;
	*result = ctx;

	return VOD_OK;
}

