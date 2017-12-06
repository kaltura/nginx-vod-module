#ifndef __HDS_MANIFEST_H__
#define __HDS_MANIFEST_H__

// includes
#include "../media_format.h"
#include "../segmenter.h"
#include "../common.h"

// typedefs
typedef struct {
	vod_str_t fragment_file_name_prefix;
	vod_str_t bootstrap_file_name_prefix;
} hds_manifest_config_t;

// functions
vod_status_t hds_packager_build_bootstrap(
	request_context_t* request_context,
	media_set_t* media_set,
	vod_str_t* result);

vod_status_t hds_packager_build_manifest(
	request_context_t* request_context,
	hds_manifest_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* manifest_id,
	media_set_t* media_set,
	bool_t drm_enabled,
	vod_str_t* result);

#endif // __HDS_MANIFEST_H__
