#ifndef __WRITE_STREAM_H__
#define __WRITE_STREAM_H__

// macros
#define write_le32(p, dw)			\
	{								\
	*(p)++ = (dw)& 0xFF;			\
	*(p)++ = ((dw) >> 8) & 0xFF;	\
	*(p)++ = ((dw) >> 16) & 0xFF;	\
	*(p)++ = ((dw) >> 24) & 0xFF;	\
	}

#define write_be16(p, w)			\
	{								\
	*(p)++ = ((w) >> 8) & 0xFF;		\
	*(p)++ = (w)& 0xFF;			\
	}

#define write_be24(p, dw)			\
	{								\
	*(p)++ = ((dw) >> 16) & 0xFF;	\
	*(p)++ = ((dw) >> 8) & 0xFF;	\
	*(p)++ = (dw)& 0xFF;			\
	}

#define write_be32(p, dw)			\
	{								\
	*(p)++ = ((dw) >> 24) & 0xFF;	\
	*(p)++ = ((dw) >> 16) & 0xFF;	\
	*(p)++ = ((dw) >> 8) & 0xFF;	\
	*(p)++ = (dw)& 0xFF;			\
	}

#define write_be64(p, qw)			\
	{								\
	write_be32(p, (qw) >> 32);		\
	write_be32(p, (qw));			\
	}

#endif //__WRITE_STREAM_H__
