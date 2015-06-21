#ifndef __MSS_PLAYREADY_H__
#define __MSS_PLAYREADY_H__

// includes
#include "../mp4/mp4_parser.h"
#include "../segmenter.h"

// functions
vod_status_t mss_playready_build_manifest(
	request_context_t* request_context,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

vod_status_t mss_playready_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size);

#endif // __MSS_PLAYREADY_H__
