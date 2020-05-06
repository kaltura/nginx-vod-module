#ifndef __DASH_PACKAGER_H__
#define __DASH_PACKAGER_H__

// includes
#include "../media_format.h"
#include "../segmenter.h"
#include "../common.h"

// constants
#define DASH_TIMESCALE (90000)

// typedefs
enum {
	FORMAT_SEGMENT_LIST,
	FORMAT_SEGMENT_TIMELINE,
	FORMAT_SEGMENT_TEMPLATE,
};

enum {
	SUBTITLE_FORMAT_WEBVTT,
	SUBTITLE_FORMAT_SMPTE_TT,
};

typedef u_char* (*dash_write_extra_traf_atoms_callback_t)(void* context, u_char* p, size_t mdat_atom_start);

typedef u_char* (*dash_write_mdat_atom_callback_t)(void* context, u_char* p);

typedef u_char* (*write_tags_callback_t)(void* context, u_char* p, media_track_t* track);

typedef struct {
	size_t size;
	write_tags_callback_t write;
	void* context;
} tags_writer_t;

typedef struct {
	vod_str_t profiles;
	vod_str_t init_file_name_prefix;
	vod_str_t fragment_file_name_prefix;
	vod_str_t subtitle_file_name_prefix;
	vod_uint_t manifest_format;
	vod_uint_t subtitle_format;
	vod_uint_t duplicate_bitrate_threshold;
	bool_t write_playready_kid;		// TODO: remove
	bool_t use_base_url_tag;		// TODO: remove - if supported by all devices, always use BaseURL
} dash_manifest_config_t;

typedef struct {
	tags_writer_t representation;
	tags_writer_t adaptation_set;
} dash_manifest_extensions_t;

typedef struct {
	size_t extra_traf_atoms_size;
	dash_write_extra_traf_atoms_callback_t write_extra_traf_atoms_callback;
	void* write_extra_traf_atoms_context;

	size_t mdat_atom_max_size;
	dash_write_mdat_atom_callback_t write_mdat_atom_callback;
	void* write_mdat_atom_context;
} dash_fragment_header_extensions_t;

// functions
vod_status_t dash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	dash_manifest_extensions_t* extensions,
	vod_str_t* result);

vod_status_t dash_packager_build_fragment_header(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	uint32_t sample_description_index,
	dash_fragment_header_extensions_t* extensions,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size);

#endif // __DASH_PACKAGER_H__
