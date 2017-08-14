#ifndef __MSS_PLAYREADY_H__
#define __MSS_PLAYREADY_H__

// includes
#include "mss_packager.h"
#include "../media_format.h"
#include "../segmenter.h"

// functions
vod_status_t mss_playready_build_manifest(
	request_context_t* request_context,
	mss_manifest_config_t* conf,
	media_set_t* media_set,
	vod_str_t* result);

vod_status_t mss_playready_get_fragment_writer(
	segment_writer_t* segment_writer,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	bool_t single_nalu_per_frame,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size);

#endif // __MSS_PLAYREADY_H__
