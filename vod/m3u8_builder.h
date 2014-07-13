#ifndef __M3U8_BUILDER_H__
#define __M3U8_BUILDER_H__

// includes
#include "mp4_parser.h"

// constants
#define MAX_ENCRYPTION_KEY_FILE_NAME_LEN (32)

static const char m3u8_header_format[] = "#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-ALLOW-CACHE:YES\n%s%s%s#EXT-X-VERSION:%d\n#EXT-X-MEDIA-SEQUENCE:1\n";
static const char encryption_key_tag_prefix[] = "#EXT-X-KEY:METHOD=AES-128,URI=\"";
static const char encryption_key_tag_postfix[] = "\"\n";
static const char m3u8_extinf_format[] = "#EXTINF:%d.%d,\n";
static const char iframes_m3u8_header_format[] = "#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-VERSION:4\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-I-FRAMES-ONLY\n";

// typedefs
typedef struct {
	int m3u8_version;
	u_char m3u8_header[sizeof(m3u8_header_format)+VOD_INT64_LEN + sizeof(encryption_key_tag_prefix)+MAX_ENCRYPTION_KEY_FILE_NAME_LEN + sizeof(encryption_key_tag_postfix)+sizeof("2")];
	size_t m3u8_header_len;
	u_char m3u8_extinf[sizeof(m3u8_extinf_format)+VOD_INT64_LEN + VOD_INT64_LEN];
	size_t m3u8_extinf_len;
	u_char iframes_m3u8_header[sizeof(iframes_m3u8_header_format)+VOD_INT64_LEN];
	size_t iframes_m3u8_header_len;
	vod_str_t segment_file_name_prefix;
} m3u8_config_t;

// functions
u_char* append_extinf_tag(u_char* p, uint32_t duration, uint32_t scale);

vod_status_t build_index_playlist_m3u8(
	request_context_t* request_context,
	m3u8_config_t* conf,
	uint32_t segment_duration,
	uint32_t clip_to,
	uint32_t clip_from,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

vod_status_t build_iframe_playlist_m3u8(
	request_context_t* request_context,
	m3u8_config_t* conf,
	uint32_t segment_duration,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result);

void init_m3u8_config(
	m3u8_config_t* conf,
	uint32_t segment_duration,
	const char* encryption_key_file_name);

#endif // __M3U8_BUILDER_H__
