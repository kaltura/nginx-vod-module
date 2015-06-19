#ifndef __DASH_PACKAGER_H__
#define __DASH_PACKAGER_H__

// includes
#include "../mp4/mp4_builder.h"
#include "../mp4/mp4_encrypt.h"
#include "../mp4/mp4_parser.h"
#include "../segmenter.h"
#include "../common.h"

// typedefs
typedef u_char* (*write_tags_callback_t)(void* context, u_char* p, mpeg_stream_metadata_t* stream);

typedef u_char* (*atom_writer_func_t)(void* context, u_char* p);

typedef struct {
	size_t atom_size;
	atom_writer_func_t write;
	void* context;
} atom_writer_t;

typedef struct {
	vod_str_t init_file_name_prefix;
	vod_str_t fragment_file_name_prefix;
	bool_t segment_timeline;
} dash_manifest_config_t;

// functions
bool_t dash_packager_compare_streams(void* context, const media_info_t* mi1, const media_info_t* mi2);

vod_status_t dash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	size_t representation_tags_size,
	write_tags_callback_t write_representation_tags,
	void* representation_tags_writer_context,
	vod_str_t* result);

vod_status_t dash_packager_build_init_mp4(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata,
	bool_t size_only,
	atom_writer_t* extra_moov_atoms_writer,
	atom_writer_t* stsd_atom_writer,
	vod_str_t* result);

vod_status_t dash_packager_build_fragment_header(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	uint32_t sample_description_index,
	size_t extra_traf_atoms_size,
	write_extra_traf_atoms_callback_t write_extra_traf_atoms_callback,
	void* write_extra_traf_atoms_context,
	atom_writer_t* mdat_prefix_writer,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size);

#endif // __DASH_PACKAGER_H__
