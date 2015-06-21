#ifndef __MSS_PACKAGER_H__
#define __MSS_PACKAGER_H__

// includes
#include "../mp4/mp4_builder.h"
#include "../mp4/mp4_encrypt.h"
#include "../mp4/mp4_parser.h"
#include "../segmenter.h"
#include "../common.h"

// constants
#define MSS_STREAM_TYPE_VIDEO "video"
#define MSS_STREAM_TYPE_AUDIO "audio"

// macros
// Note: in order to be able to process fragment requests efficiently, we need to know the file index and track index
//		of the fragment. since we only have the bitrate on the URL, we encode this parameters on the bitrate.
//		since both parameters are limited to 32, this results in a maximum of 1kpbs diviation from the real bitrate.
#define mss_encode_indexes(bitrate, file_index, track_index) (((bitrate) & ~0x3FF) | (((file_index) & 0x1F) << 5) | ((track_index) & 0x1F))
#define mss_file_index(bitrate)	(((bitrate) >> 5) & 0x1F)
#define mss_track_index(bitrate)	((bitrate) & 0x1F)

//typedefs
typedef u_char* (*mss_write_tags_callback_t)(void* context, u_char* p, mpeg_metadata_t* stream);

// functions
bool_t mss_packager_compare_streams(void* context, const media_info_t* mi1, const media_info_t* mi2);

vod_status_t mss_packager_build_manifest(
	request_context_t* request_context,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	size_t extra_tags_size,
	mss_write_tags_callback_t write_extra_tags,
	void* extra_tags_writer_context,
	vod_str_t* result);

vod_status_t mss_packager_build_fragment_header(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	size_t extra_traf_atoms_size,
	write_extra_traf_atoms_callback_t write_extra_traf_atoms_callback,
	void* write_extra_traf_atoms_context,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size);

#endif // __MSS_PACKAGER_H__
