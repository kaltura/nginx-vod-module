#ifndef __COMMON_H__
#define __COMMON_H__

// includes
#include <inttypes.h>
#include <sys/types.h>

// constants
#ifndef TRUE
#define TRUE (1)
#endif // TRUE

#ifndef FALSE
#define FALSE (0)
#endif // FALSE

#define INVALID_FILE_INDEX ((uint32_t)-1)

// macros
#define vod_div_ceil(x, y) (((x) + (y) - 1) / (y))
#define vod_array_entries(x) (sizeof(x) / sizeof(x[0]))

#ifdef VOD_STAND_ALONE

// includes
#include <stdlib.h>
#include <string.h>

// macros
#define vod_min(x, y) (((x) < (y)) ? (x) : (y))
#define vod_max(x, y) (((x) > (y)) ? (x) : (y))

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif // offsetof

// error codes
#define  VOD_OK          0
#define  VOD_AGAIN      -2

// memory set/copy functions
#define vod_memcpy(dst, src, n) memcpy(dst, src, n)
#define vod_memmove(dst, src, n) memmove(dst, src, n)
#define vod_memset(buf, c, n) memset(buf, c, n)
#define vod_memzero(buf, n) memset(buf, 0, n)

// memory alloc functions
#define vod_alloc(pool, size) malloc(size)
#define vod_free(pool, ptr) free(ptr)

#include "vod_array.h"

#else	// VOD_STAND_ALONE

// includes
#include <ngx_core.h>

#define VOD_INT64_LEN NGX_INT64_LEN
#define VOD_INT32_LEN NGX_INT32_LEN

#define VOD_HAVE_LIB_AV_CODEC NGX_HAVE_LIB_AV_CODEC 
#define VOD_HAVE_LIB_AV_FILTER NGX_HAVE_LIB_AV_FILTER

// macros
#define vod_min(x, y) ngx_min(x, y)
#define vod_max(x, y) ngx_max(x, y)

// errors codes
#define  VOD_OK         NGX_OK
#define  VOD_AGAIN      NGX_AGAIN

#define vod_inline ngx_inline

// memory set/copy functions
#define vod_memcpy(dst, src, n) ngx_memcpy(dst, src, n)
#define vod_memmove(dst, src, n) ngx_memmove(dst, src, n)
#define vod_memset(buf, c, n) ngx_memset(buf, c, n)
#define vod_memzero(buf, n) ngx_memzero(buf, n)
#define vod_memcmp(s1, s2, n) ngx_memcmp(s1, s2, n)
#define vod_copy(dst, src, n) ngx_copy(dst, src, n)

// memory alloc functions
#define vod_memalign(pool, size, alignment) ngx_pmemalign(pool, size, alignment)
#define vod_alloc(pool, size) ngx_palloc(pool, size)
#define vod_free(pool, ptr) ngx_pfree(pool, ptr)

// string functions
#define vod_sprintf ngx_sprintf
#define vod_snprintf ngx_snprintf
#define vod_atoi(str, len) ngx_atoi(str, len)

// array functions
#define vod_array_init(array, pool, n, size) ngx_array_init(array, pool, n, size)
#define vod_array_push(array) ngx_array_push(array)
#define vod_array_push_n(array, count) ngx_array_push_n(array, count)
#define vod_array_destroy(a) ngx_array_destroy(array)

// types
#define vod_array_t ngx_array_t
#define vod_pool_t ngx_pool_t
#define vod_log_t ngx_log_t
#define vod_str_t ngx_str_t
#define vod_buf_t ngx_buf_t
#define vod_chain_t ngx_chain_t
#define vod_string(str) ngx_string(str)
#define vod_encode_base64(base64, binary) ngx_encode_base64(base64, binary)
#define vod_base64_encoded_length(len) ngx_base64_encoded_length(len)

#define VOD_LOG_STDERR            NGX_LOG_STDERR 
#define VOD_LOG_EMERG             NGX_LOG_EMERG  
#define VOD_LOG_ALERT             NGX_LOG_ALERT  
#define VOD_LOG_CRIT              NGX_LOG_CRIT   
#define VOD_LOG_ERR               NGX_LOG_ERR    
#define VOD_LOG_WARN              NGX_LOG_WARN   
#define VOD_LOG_NOTICE            NGX_LOG_NOTICE 
#define VOD_LOG_INFO              NGX_LOG_INFO   
#define VOD_LOG_DEBUG             NGX_LOG_DEBUG  

#define vod_log_error ngx_log_error

#define VOD_LOG_DEBUG_LEVEL (NGX_LOG_DEBUG_HTTP)

#define vod_log_debug0(level, log, err, fmt) \
        ngx_log_debug0(level, log, err, fmt)

#define vod_log_debug1(level, log, err, fmt, arg1) \
        ngx_log_debug1(level, log, err, fmt, arg1)

#define vod_log_debug2(level, log, err, fmt, arg1, arg2) \
        ngx_log_debug2(level, log, err, fmt, arg1, arg2)

#define vod_log_debug3(level, log, err, fmt, arg1, arg2, arg3) \
        ngx_log_debug3(level, log, err, fmt, arg1, arg2, arg3)

#define vod_log_debug4(level, log, err, fmt, arg1, arg2, arg3, arg4) \
		ngx_log_debug4(level, log, err, fmt, arg1, arg2, arg3, arg4)

#if (NGX_DEBUG)
#define VOD_DEBUG (1)
#else
#define VOD_DEBUG (0)
#endif

#endif	// VOD_STAND_ALONE

#if (VOD_DEBUG)

#define vod_log_buffer(level, log, err, prefix, buffer, size)	\
	if ((log)->log_level & level)								\
		log_buffer(level, log, err, prefix, buffer, size)
		
#define MAX_DUMP_BUFFER_SIZE (100)

static vod_inline void 
log_buffer(unsigned level, vod_log_t* log, int err, const char* prefix, const u_char* buffer, int size)
{
	static const char hex_chars[] = "0123456789abcdef";
	char hex[MAX_DUMP_BUFFER_SIZE * 3 + 1];
	char* hex_pos = hex;
	
	size = vod_min(size, MAX_DUMP_BUFFER_SIZE);
	for (; size > 0; size--, buffer++)
	{
		*hex_pos++ = hex_chars[*buffer >> 4];
		*hex_pos++ = hex_chars[*buffer & 0xF];
		*hex_pos++ = ' ';
	}
	*hex_pos = '\0';
	
	vod_log_debug2(level, log, err, "%s %s", prefix, hex);
}

#else	// VOD_DEBUG

#define vod_log_buffer(level, log, err, prefix, buffer, size)

#endif	// VOD_DEBUG

enum {
	VOD_ERROR_FIRST = -1000,
	VOD_BAD_DATA = VOD_ERROR_FIRST,
	VOD_ALLOC_FAILED,
	VOD_UNEXPECTED,
	VOD_BAD_REQUEST,
	VOD_ERROR_LAST,
};

typedef intptr_t bool_t;
typedef intptr_t vod_status_t;

struct media_info_s;
typedef struct media_info_s media_info_t;
typedef bool_t(*stream_comparator_t)(void* context, const media_info_t* mi1, const media_info_t* mi2);

typedef vod_status_t(*write_callback_t)(void* context, u_char* buffer, uint32_t size, bool_t* reuse_buffer);

typedef struct {
	write_callback_t write_tail;
	write_callback_t write_head;
	void* context;
} segment_writer_t;

typedef struct {
	vod_pool_t* pool;
	vod_log_t *log;
	int parse_type;
	stream_comparator_t stream_comparator;
	void* stream_comparator_context;
	uint64_t start;
	uint64_t end;
	uint32_t timescale;
	uint32_t max_frame_count;
	bool_t simulation_only;
} request_context_t;

#endif // __COMMON_H__
