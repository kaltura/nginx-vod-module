#ifndef __COMMON_H__
#define __COMMON_H__

// constants
#ifndef TRUE
#define TRUE (1)
#endif // TRUE

#ifndef FALSE
#define FALSE (0)
#endif // FALSE

#define VOD_GUID_SIZE (16)

// macros
#define vod_div_ceil(x, y) (((x) + (y) - 1) / (y))
#define vod_array_entries(x) (sizeof(x) / sizeof(x[0]))

#ifdef VOD_STAND_ALONE

// includes
#include <inttypes.h>
#include <sys/types.h>
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

#define VOD_LOG_STDERR            1
#define VOD_LOG_EMERG             2
#define VOD_LOG_ALERT             3
#define VOD_LOG_CRIT              4
#define VOD_LOG_ERR               5
#define VOD_LOG_WARN              6
#define VOD_LOG_NOTICE            7
#define VOD_LOG_INFO              8

#define VOD_LOG_DEBUG_LEVEL (0x100)

#define vod_log_debug0(level, log, err, fmt)
#define vod_log_debug1(level, log, err, fmt, arg1)
#define vod_log_debug2(level, log, err, fmt, arg1, arg2)
#define vod_log_debug3(level, log, err, fmt, arg1, arg2, arg3)
#define vod_log_debug4(level, log, err, fmt, arg1, arg2, arg3, arg4)

typedef int bool_t;
typedef int vod_status_t;
typedef unsigned int vod_uint_t;
typedef void vod_pool_t;
typedef void vod_log_t;

void vod_log_error(vod_uint_t level, vod_log_t *log, int err,
 const char *fmt, ...);

#else	// VOD_STAND_ALONE

// includes
#include <ngx_core.h>
#include <inttypes.h>

#define VOD_INT64_LEN NGX_INT64_LEN
#define VOD_INT32_LEN NGX_INT32_LEN
#define VOD_MAX_SIZE_T_VALUE NGX_MAX_SIZE_T_VALUE
#define VOD_MAX_OFF_T_VALUE NGX_MAX_OFF_T_VALUE

#define VOD_HAVE_LIB_AV_CODEC NGX_HAVE_LIB_AV_CODEC 
#define VOD_HAVE_LIB_AV_FILTER NGX_HAVE_LIB_AV_FILTER
#define VOD_HAVE_OPENSSL_EVP NGX_HAVE_OPENSSL_EVP

// macros
#define vod_container_of(ptr, type, member) (type *)((char *)(ptr) - offsetof(type, member))
#define vod_min(x, y) ngx_min(x, y)
#define vod_max(x, y) ngx_max(x, y)

// errors codes
#define  VOD_OK         NGX_OK
#define  VOD_DONE       NGX_DONE
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
#define vod_alloc(pool, size) ngx_palloc(pool, size)
#define vod_free(pool, ptr) ngx_pfree(pool, ptr)
#define vod_pool_cleanup_add(pool, size) ngx_pool_cleanup_add(pool, size)
#define vod_align(d, a) ngx_align(d, a)

// string functions
#define vod_sprintf ngx_sprintf
#define vod_snprintf ngx_snprintf
#define vod_strncmp(s1, s2, n) ngx_strncmp(s1, s2, n)
#define vod_strncasecmp(s1, s2, n) ngx_strncasecmp(s1, s2, n)
#define vod_atoi(str, len) ngx_atoi(str, len)
#define vod_atofp(str, len, point) ngx_atofp(str, len, point)

// array functions
#define vod_array_init(array, pool, n, size) ngx_array_init(array, pool, n, size)
#define vod_array_push(array) ngx_array_push(array)
#define vod_array_push_n(array, count) ngx_array_push_n(array, count)
#define vod_array_destroy(a) ngx_array_destroy(array)

// hash functions
#define vod_hash(key, c) ngx_hash(key, c)
#define vod_hash_key_lc ngx_hash_key_lc
#define vod_cacheline_size ngx_cacheline_size
#define vod_hash_init(hinit, names, nelts) ngx_hash_init(hinit, names, nelts)
#define vod_hash_find(hash, key, name, len) ngx_hash_find(hash, key, name, len)

// time functions
#define vod_time() ngx_time()
#define vod_gmtime(t, tp) ngx_gmtime(t, tp)
#define vod_tm_sec   ngx_tm_sec
#define vod_tm_min   ngx_tm_min
#define vod_tm_hour  ngx_tm_hour
#define vod_tm_mday  ngx_tm_mday
#define vod_tm_mon   ngx_tm_mon
#define vod_tm_year  ngx_tm_year
#define vod_tm_wday  ngx_tm_wday
#define vod_tm_isdst ngx_tm_isdst

// types
#define vod_hash_t ngx_hash_t
#define vod_hash_key_t ngx_hash_key_t
#define vod_hash_init_t ngx_hash_init_t
#define vod_array_t ngx_array_t
#define vod_pool_t ngx_pool_t
#define vod_pool_cleanup_t ngx_pool_cleanup_t
#define vod_pool_cleanup_pt ngx_pool_cleanup_pt
#define vod_log_t ngx_log_t
#define vod_str_t ngx_str_t
#define vod_buf_t ngx_buf_t
#define vod_chain_t ngx_chain_t
#define vod_tm_t ngx_tm_t

#define vod_string(str) ngx_string(str)
#define vod_null_string ngx_null_string
#define vod_encode_base64(base64, binary) ngx_encode_base64(base64, binary)
#define vod_decode_base64(binary, base64) ngx_decode_base64(binary, base64)
#define vod_base64_encoded_length(len) ngx_base64_encoded_length(len)
#define vod_base64_decoded_length(len) ngx_base64_decoded_length(len)

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

typedef intptr_t bool_t;
typedef ngx_int_t vod_status_t;
typedef ngx_int_t vod_int_t;
typedef ngx_uint_t vod_uint_t;

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
	VOD_BAD_MAPPING,
	VOD_NOT_FOUND,
	VOD_ERROR_LAST,
};

typedef vod_status_t(*write_callback_t)(void* context, u_char* buffer, uint32_t size);

typedef struct {
	write_callback_t write_tail;
	write_callback_t write_head;
	void* context;
} segment_writer_t;

struct buffer_pool_s;
typedef struct buffer_pool_s buffer_pool_t;

typedef struct {
	vod_pool_t* pool;
	vod_log_t *log;
	buffer_pool_t* output_buffer_pool;
	bool_t simulation_only;
} request_context_t;

enum {
	MEDIA_TYPE_VIDEO,
	MEDIA_TYPE_AUDIO,
	MEDIA_TYPE_COUNT,
	MEDIA_TYPE_NONE,
};

// functions
int vod_get_int_print_len(uint64_t n);

uint32_t vod_get_number_of_set_bits(uint32_t i);

#endif // __COMMON_H__
