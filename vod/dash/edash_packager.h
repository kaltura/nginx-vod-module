#ifndef __EDASH_PACKAGER_H__
#define __EDASH_PACKAGER_H__

// includes
#include "dash_packager.h"
#include "../udrm.h"

// constants
#define EDASH_INIT_MP4_HAS_CLEAR_LEAD	(0x01)
#define EDASH_INIT_MP4_WRITE_PSSH		(0x02)

// functions
vod_status_t edash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	bool_t drm_single_key,
	vod_str_t* result);

vod_status_t edash_packager_build_init_mp4(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	bool_t size_only,
	vod_str_t* result);

vod_status_t edash_packager_get_fragment_writer(
	segment_writer_t* segment_writer,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	bool_t single_nalu_per_frame,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size);

u_char* edash_packager_write_pssh(
	u_char* p,
	drm_system_info_t* cur_info);

#endif //__EDASH_PACKAGER_H__
