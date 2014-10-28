#ifndef __HDS_MANIFEST_H__
#define __HDS_MANIFEST_H__

// includes
#include "../mp4_builder.h"
#include "../mp4_parser.h"
#include "../common.h"

// typedefs
typedef struct {
	vod_str_t fragment_file_name_prefix;
} hds_manifest_config_t;

// functions
vod_status_t hds_packager_build_manifest(
	request_context_t* request_context,
	hds_manifest_config_t* conf,
	vod_str_t* manifest_id,
	uint32_t segment_duration,
	bool_t include_file_index,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

#endif // __HDS_MANIFEST_H__
