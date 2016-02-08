#ifndef __MP4_PARSER_BASE_H__
#define __MP4_PARSER_BASE_H__

#include "../media_format.h"
#include "mp4_defs.h"

// atom parsing types
typedef uint32_t atom_name_t;

typedef struct {
	const u_char* ptr;
	uint64_t size;
	atom_name_t name;
	uint8_t header_size;
} atom_info_t;

typedef vod_status_t(*parse_atoms_callback_t)(void* context, atom_info_t* atom_info);

// relevant atoms finder
typedef struct relevant_atom_s {
	atom_name_t atom_name;
	int atom_info_offset;
	const struct relevant_atom_s* relevant_children;
} relevant_atom_t;

typedef struct {
	request_context_t* request_context;
	const relevant_atom_t* relevant_atoms;
	void* result;
} save_relevant_atoms_context_t;

// iterators
typedef struct {
	stts_entry_t* last_entry;
	stts_entry_t* cur_entry;
	uint32_t sample_count;
	uint64_t accum_duration;
	uint32_t frame_index;
} stts_iterator_state_t;

typedef struct {
	ctts_entry_t* last_entry;
	ctts_entry_t* cur_entry;
	uint32_t sample_count;
	uint32_t frame_index;
} ctts_iterator_state_t;

typedef struct {
	request_context_t* request_context;
	stsc_entry_t* last_entry;
	uint32_t chunks;

	stsc_entry_t* cur_entry;
	uint32_t cur_chunk;
	uint32_t samples_per_chunk;
	uint32_t sample_desc;
	uint32_t frame_index;
} stsc_iterator_state_t;

// basic parsing functions
vod_status_t mp4_parser_parse_atoms(
	request_context_t* request_context,
	const u_char* buffer,
	uint64_t buffer_size,
	bool_t validate_full_atom,
	parse_atoms_callback_t callback,
	void* context);

vod_status_t mp4_parser_save_relevant_atoms_callback(void* ctx, atom_info_t* atom_info);

// validation functions
vod_status_t mp4_parser_validate_stts_data(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries);

vod_status_t mp4_parser_validate_stss_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries);

vod_status_t mp4_parser_validate_ctts_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries);

vod_status_t mp4_parser_validate_stsc_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries);

vod_status_t mp4_parser_validate_stsz_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t last_frame,
	uint32_t* uniform_size,
	uint32_t* field_size,
	uint32_t* entries);

vod_status_t mp4_parser_validate_stco_data(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t last_chunk_index,
	uint32_t* entries,
	uint32_t* entry_size);

// iterators

// stts
void mp4_parser_stts_iterator_init(
	stts_iterator_state_t* iterator, 
	media_parse_params_t* parse_params,
	stts_entry_t* first_entry, 
	uint32_t entries);

bool_t mp4_parser_stts_iterator(
	stts_iterator_state_t* iterator,
	uint64_t offset);

// stss
uint32_t mp4_parser_find_stss_entry(
	uint32_t frame_index,
	const uint32_t* first_entry,
	uint32_t entries);

// ctts
void mp4_parser_ctts_iterator_init(
	ctts_iterator_state_t* iterator,
	ctts_entry_t* first_entry,
	uint32_t entries);

bool_t mp4_parser_ctts_iterator(
	ctts_iterator_state_t* iterator,
	uint32_t required_index);

// stsc
vod_status_t mp4_parser_stsc_iterator_init(
	stsc_iterator_state_t* iterator,
	request_context_t* request_context,
	stsc_entry_t* first_entry,
	uint32_t entries, 
	uint32_t chunks);

vod_status_t mp4_parser_stsc_iterator(
	stsc_iterator_state_t* iterator,
	uint32_t required_index,
	uint32_t* target_chunk,
	uint32_t* sample_count, 
	uint32_t* next_chunk_out, 
	uint32_t* prev_samples);

#endif // __MP4_PARSER_BASE_H__
