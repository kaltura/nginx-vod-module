#ifndef __MP4_WRITE_STREAM_H__
#define __MP4_WRITE_STREAM_H__

#include "../write_stream.h"

// macros
#define write_atom_name(p, c1, c2, c3, c4) \
	{ *(p)++ = (c1); *(p)++ = (c2); *(p)++ = (c3); *(p)++ = (c4); }

#define write_atom_header(p, size, c1, c2, c3, c4) \
	{										\
	write_be32(p, size);					\
	write_atom_name(p, c1, c2, c3, c4);		\
	}

#define write_atom_header64(p, size, c1, c2, c3, c4) \
	{										\
	write_be32(p, 1);						\
	write_atom_name(p, c1, c2, c3, c4);		\
	write_be64(p, size);					\
	}

#endif //__MP4_WRITE_STREAM_H__
