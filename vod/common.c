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
