#include "../media_format.h"
#include "../mp4/mp4_defs.h"
#include "mkv_defs.h"

// constants
mkv_codec_type_t mkv_codec_types[] = {
	// video
	{ vod_string("V_MPEG4/ISO/AVC"),	VOD_CODEC_ID_AVC,	FORMAT_AVC1,	TRUE },
	{ vod_string("V_MPEGH/ISO/HEVC"),	VOD_CODEC_ID_HEVC,	FORMAT_HVC1,	TRUE },
	{ vod_string("V_VP8"),				VOD_CODEC_ID_VP8,	0,				FALSE },
	{ vod_string("V_VP9"),				VOD_CODEC_ID_VP9,	0,				FALSE },
	{ vod_string("V_AV1"),				VOD_CODEC_ID_AV1,	0,				FALSE },

	// audio
	{ vod_string("A_AAC"),				VOD_CODEC_ID_AAC,	FORMAT_MP4A,	TRUE },
	{ vod_string("A_MPEG/L3"),			VOD_CODEC_ID_MP3,	FORMAT_MP4A,	FALSE },
	{ vod_string("A_VORBIS"),			VOD_CODEC_ID_VORBIS,0,				TRUE },
	{ vod_string("A_OPUS"),				VOD_CODEC_ID_OPUS,	FORMAT_OPUS,	TRUE },
	{ vod_string("A_AC3"),				VOD_CODEC_ID_AC3,	FORMAT_AC3,		FALSE },
	{ vod_string("A_EAC3"),				VOD_CODEC_ID_EAC3,	FORMAT_EAC3,	FALSE },
	{ vod_string("A_DTS"),				VOD_CODEC_ID_DTS,	0,				TRUE },
	{ vod_string("A_FLAC"),				VOD_CODEC_ID_FLAC,	FORMAT_FLAC,	TRUE },

	{ vod_null_string, 0, 0, FALSE }
};
