#ifndef __PARSE_UTILS_H__
#define __PARSE_UTILS_H__

// includes
#include "common.h"
#include "media_format.h"

// functions
vod_status_t parse_utils_parse_guid_string(vod_str_t* str, u_char* output);

vod_status_t parse_utils_parse_fixed_base64_string(vod_str_t* str, u_char* output, size_t output_size);

vod_status_t parse_utils_parse_variable_base64_string(vod_pool_t* pool, vod_str_t* str, vod_str_t* result);

u_char* parse_utils_extract_uint32_token(u_char* start_pos, u_char* end_pos, uint32_t* result);

u_char* parse_utils_extract_track_tokens(u_char* start_pos, u_char* end_pos, track_mask_t* result);

#endif // __PARSE_UTILS_H__
