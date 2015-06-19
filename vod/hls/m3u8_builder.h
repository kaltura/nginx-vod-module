#ifndef __M3U8_BUILDER_H__
#define __M3U8_BUILDER_H__

// includes
#include "../mp4/mp4_parser.h"
#include "../segmenter.h"

// constants
#define MAX_IFRAMES_M3U8_HEADER_SIZE (sizeof(iframes_m3u8_header_format) + VOD_INT64_LEN)
#define MAX_EXTINF_SIZE (sizeof(m3u8_extinf_format) + 2 * VOD_INT64_LEN)
	
static const char encryption_key_tag_prefix[] = "#EXT-X-KEY:METHOD=AES-128,URI=\"";
static const char encryption_key_tag_postfix[] = ".key\"\n";
static const char m3u8_extinf_format[] = "#EXTINF:%d.%d,\n";
static const char iframes_m3u8_header_format[] = "#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-VERSION:4\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-I-FRAMES-ONLY\n";

// typedefs
typedef struct {
	int m3u8_version;
	u_char iframes_m3u8_header[MAX_IFRAMES_M3U8_HEADER_SIZE];
	size_t iframes_m3u8_header_len;
	vod_str_t index_file_name_prefix;
	vod_str_t segment_file_name_prefix;
	vod_str_t encryption_key_file_name;
} m3u8_config_t;

// functions
vod_status_t m3u8_builder_build_master_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	bool_t include_file_index,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

vod_status_t m3u8_builder_build_index_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* segments_base_url,
	bool_t include_file_index,
	bool_t encryption_enabled,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

vod_status_t m3u8_builder_build_iframe_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	bool_t include_file_index,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

void m3u8_builder_init_config(
	m3u8_config_t* conf,
	uint32_t max_segment_duration);

#endif // __M3U8_BUILDER_H__
