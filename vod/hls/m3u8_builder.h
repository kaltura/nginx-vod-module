#ifndef __M3U8_BUILDER_H__
#define __M3U8_BUILDER_H__

// includes
#include "../media_format.h"
#include "../segmenter.h"
#include "hls_muxer.h"

// constants
#define MAX_IFRAMES_M3U8_HEADER_SIZE (sizeof(iframes_m3u8_header_format) + VOD_INT64_LEN)
	
static const char iframes_m3u8_header_format[] = "#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-VERSION:4\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-I-FRAMES-ONLY\n";

// typedefs
enum {
	HLS_CONTAINER_AUTO,
	HLS_CONTAINER_MPEGTS,
	HLS_CONTAINER_FMP4,
};

typedef struct {
	int m3u8_version;
	vod_uint_t container_format;
	u_char iframes_m3u8_header[MAX_IFRAMES_M3U8_HEADER_SIZE];
	size_t iframes_m3u8_header_len;
	bool_t force_unmuxed_segments;
	bool_t output_iframes_playlist;
	vod_str_t index_file_name_prefix;
	vod_str_t iframes_file_name_prefix;
	vod_str_t segment_file_name_prefix;
	vod_str_t init_file_name_prefix;
	vod_str_t encryption_key_file_name;
	vod_str_t encryption_key_format;
	vod_str_t encryption_key_format_versions;
} m3u8_config_t;

// functions
vod_status_t m3u8_builder_build_master_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_uint_t encryption_method,
	vod_str_t* base_url,
	media_set_t* media_set,
	vod_str_t* result);

vod_status_t m3u8_builder_build_index_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* segments_base_url,
	hls_encryption_params_t* encryption_params,
	vod_uint_t container_format,
	media_set_t* media_set,
	vod_str_t* result);

vod_status_t m3u8_builder_build_iframe_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	hls_mpegts_muxer_conf_t* muxer_conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	vod_str_t* result);

void m3u8_builder_init_config(
	m3u8_config_t* conf,
	uint32_t max_segment_duration,
	hls_encryption_type_t encryption_method);

#endif // __M3U8_BUILDER_H__
