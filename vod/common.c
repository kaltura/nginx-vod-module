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

uint32_t
vod_get_number_of_set_bits(uint32_t i)
{
	// variable-precision SWAR algorithm
	i = i - ((i >> 1) & 0x55555555);
	i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
	return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}
