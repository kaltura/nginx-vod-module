#ifndef _NGX_SIMPLE_JSON_PARSER_H_INCLUDED_
#define _NGX_SIMPLE_JSON_PARSER_H_INCLUDED_

// includes
#include <ngx_core.h>

// macros
#define NGX_GUID_LEN (16)

// typedefs
enum {
	NGX_JSON_NULL,
	NGX_JSON_TRUE,
	NGX_JSON_FALSE,
	NGX_JSON_STRING,
	NGX_JSON_ARRAY,
	NGX_JSON_OBJECT,
};

enum {
	NGX_JSON_OK = 0,
	NGX_JSON_BAD_DATA = -1,
	NGX_JSON_ALLOC_FAILED = -2,
	NGX_JSON_BAD_LENGTH = -3,
	NGX_JSON_BAD_TYPE = -4,
	NGX_JSON_NOT_FOUND = -5,
};

typedef struct {
	int type;
	union {
		ngx_str_t str;			// Note: the string is not unescaped (e.g. may contain \n, \t etc.)
		ngx_array_t arr;
		ngx_array_t obj;		// Note: a more efficient implementation can use hash table for objects, 
								//		but since we work with very small jsons, it doesn't matter
	} v;
} ngx_json_value_t;

typedef struct {
	ngx_str_t key;
	ngx_json_value_t value;
} ngx_json_key_value_t;

// functions
ngx_int_t ngx_json_parse(ngx_pool_t* pool, u_char* string, ngx_json_value_t* result);

ngx_int_t ngx_json_get_element_guid_string(ngx_json_value_t* object, ngx_str_t* key, u_char* output);

ngx_int_t ngx_json_get_element_fixed_binary_string(ngx_json_value_t* object, ngx_str_t* key, u_char* output, size_t output_size);

ngx_int_t ngx_json_get_element_binary_string(ngx_pool_t* pool, ngx_json_value_t* object, ngx_str_t* key, ngx_str_t* result);

ngx_int_t ngx_json_get_element_array(ngx_json_value_t* object, ngx_str_t* key, ngx_array_t** result);

#endif // _NGX_SIMPLE_JSON_PARSER_H_INCLUDED_
