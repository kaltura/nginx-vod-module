#include "common.h"

int
vod_get_int_print_len(uint64_t n)
{
	int res = 1;
	while (n >= 10)
	{
		res++;
		n /= 10;
	}
	return res;
}

#ifdef VOD_IMPLEMENT_BIT_COUNT
uint32_t
vod_get_number_of_set_bits32(uint32_t i)
{
	// variable-precision SWAR algorithm
	i = i - ((i >> 1) & 0x55555555);
	i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
	return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

uint32_t
vod_get_number_of_set_bits64(uint64_t i)
{
	return vod_get_number_of_set_bits32((uint32_t)(i & NGX_MAX_UINT32_VALUE)) +
		vod_get_number_of_set_bits32((uint32_t)(i >> 32));
}

uint32_t
vod_get_trailing_zeroes64(uint64_t i)
{
	size_t k;

	for (k = 0; k < sizeof(i) * 8; k++)
	{
		if ((i >> k) & 1)
		{
			return k;
		}
	}
	return -1; /* undefined */
}
#endif

u_char*
vod_append_hex_string(u_char* p, const u_char* buffer, uint32_t buffer_size)
{
	const u_char* buffer_end = buffer + buffer_size;
	static const u_char hex_chars[] = "0123456789ABCDEF";

	for (; buffer < buffer_end; buffer++)
	{
		*p++ = hex_chars[*buffer >> 4];
		*p++ = hex_chars[*buffer & 0x0F];
	}
	return p;
}
