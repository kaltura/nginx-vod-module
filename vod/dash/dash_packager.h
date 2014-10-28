#ifndef __DASH_PACKAGER_H__
#define __DASH_PACKAGER_H__

// includes
#include "../mp4_builder.h"
#include "../mp4_parser.h"
#include "../common.h"

// typedefs
typedef struct {
	vod_str_t init_file_name_prefix;
	vod_str_t fragment_file_name_prefix;
} dash_manifest_config_t;

// functions
bool_t dash_packager_compare_streams(void* context, const media_info_t* mi1, const media_info_t* mi2);

vod_status_t dash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	uint32_t segment_duration,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

vod_status_t dash_packager_build_init_mp4(
	request_context_t* request_context, 
	mpeg_metadata_t* mpeg_metadata, 
	bool_t size_only,
	vod_str_t* result);

vod_status_t dash_packager_build_fragment_header(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	uint32_t segment_duration,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size);

#endif // __DASH_PACKAGER_H__
