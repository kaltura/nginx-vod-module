#ifndef __EBML_H__
#define __EBML_H__

#include "../common.h"

// macros
#define ebml_read_id(context, id) ebml_read_num(context, id, 4, 0)
#define is_unknown_size(num, num_bytes) ((num) + 1 == 1ULL << (7 * (num_bytes)))

#define EBML_TRUNCATE_SIZE 0x80

// typedefs
typedef enum {
	EBML_NONE,
	EBML_UINT,
	EBML_FLOAT,
	EBML_STRING,
	EBML_BINARY,
	EBML_MASTER,
	EBML_CUSTOM,
} ebml_type_t;

typedef struct {
	request_context_t* request_context;
	const u_char* cur_pos;
	const u_char* end_pos;
	int64_t offset_delta;
} ebml_context_t;

typedef struct {
	uint32_t id;
	ebml_type_t type;
	off_t offset;
	void* child;
} ebml_spec_t;

typedef struct {
	uint64_t version;
	uint64_t max_size;
	uint64_t id_length;
	vod_str_t doctype;
	uint64_t doctype_version;
} ebml_header_t;

typedef vod_status_t (*ebml_parser_t)(ebml_context_t* context, ebml_spec_t* spec, void* dest);

// functions
vod_status_t ebml_read_num(ebml_context_t* context, uint64_t* result, size_t max_size, int remove_first_bit);

vod_status_t ebml_parse_header(ebml_context_t* context, ebml_header_t* header);

vod_status_t ebml_parse_single(ebml_context_t* context, ebml_spec_t* spec, void* dest);

vod_status_t ebml_parse_master(ebml_context_t* context, ebml_spec_t* spec, void* dest);

#endif //__EBML_H__
