#ifndef __EDASH_PACKAGER_H__
#define __EDASH_PACKAGER_H__

// includes
#include "dash_packager.h"

// functions
vod_status_t edash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

vod_status_t edash_packager_build_init_mp4(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata,
	bool_t has_clear_lead,
	bool_t size_only,
	vod_str_t* result);

vod_status_t edash_packager_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size);

#endif //__EDASH_PACKAGER_H__
