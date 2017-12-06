#ifndef __JSON_PARSER_H__
#define __JSON_PARSER_H__

// includes
#include "common.h"

// enums
enum {
	VOD_JSON_NULL,
	VOD_JSON_BOOL,
	VOD_JSON_INT,
	VOD_JSON_FRAC,
	VOD_JSON_STRING,
	VOD_JSON_ARRAY,
	VOD_JSON_OBJECT,
};

enum {
	VOD_JSON_OK = 0,
	VOD_JSON_BAD_DATA = -1,
	VOD_JSON_ALLOC_FAILED = -2,
	VOD_JSON_BAD_LENGTH = -3,
	VOD_JSON_BAD_TYPE = -4,
};

// typedefs
typedef vod_status_t vod_json_status_t;

typedef struct {
	int64_t num;
	uint64_t denom;
} vod_json_fraction_t;

typedef struct {
	int type;
	size_t count;
	vod_array_part_t part;
} vod_json_array_t;

typedef vod_array_t vod_json_object_t;

typedef struct {
	int type;
	union {
		bool_t boolean;
		vod_json_fraction_t num;
		vod_str_t str;			// Note: the string is not unescaped (e.g. may contain \n, \t etc.)
		vod_json_array_t arr;
		vod_json_object_t obj;	// of vod_json_key_value_t
	} v;
} vod_json_value_t;

typedef struct {
	vod_uint_t key_hash;
	vod_str_t key;
	vod_json_value_t value;
} vod_json_key_value_t;

// functions
vod_json_status_t vod_json_parse(
	vod_pool_t* pool, 
	u_char* string, 
	vod_json_value_t* result, 
	u_char* error, 
	size_t error_size);

vod_json_status_t vod_json_decode_string(vod_str_t* dest, vod_str_t* src);

vod_status_t vod_json_init_hash(
	vod_pool_t* pool,
	vod_pool_t* temp_pool,
	char* hash_name,
	void* elements,
	size_t element_size,
	vod_hash_t* result);

// key extraction - use when the fields have to be parsed in a certain order
typedef struct {
	vod_str_t key;
	int type;
	int index;
} json_object_key_def_t;

void vod_json_get_object_values(
	vod_json_object_t* object,
	vod_hash_t* values_hash,
	vod_json_value_t** result);

// value parsing - use when the fields can be parsed in any order, make sure to check for mandatory fields
typedef vod_status_t (*vod_json_object_value_parser_t)(
	void* context,
	vod_json_value_t* value,
	void* dest);

typedef struct {
	vod_str_t key;
	int type;
	size_t offset;
	vod_json_object_value_parser_t parse;
} json_object_value_def_t;

vod_status_t vod_json_parse_object_values(
	vod_json_object_t* object,
	vod_hash_t* values_hash,
	void* context,
	void* result);

// union parsing
typedef vod_status_t(*json_parser_union_type_parser_t)(
	void* context,
	vod_json_object_t* object,
	void** dest);

typedef struct {
	vod_str_t type;
	json_parser_union_type_parser_t parser;
} json_parser_union_type_def_t;

vod_status_t vod_json_parse_union(
	request_context_t* request_context,
	vod_json_object_t* object,
	vod_str_t* type_field,
	vod_uint_t type_field_hash,
	vod_hash_t* union_hash,
	void* context,
	void** dest);

// misc
vod_status_t vod_json_replace(
	vod_json_value_t* json1,
	vod_json_value_t* json2);

#endif // __JSON_PARSER_H__
