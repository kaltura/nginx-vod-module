#ifndef __MKV_DEFS_H__
#define __MKV_DEFS_H__

#include "../common.h"

// constants
#define NANOS_PER_SEC (1000000000)

// Matroska elements

// seek
#define MKV_ID_SEEKID				(0x53AB)
#define MKV_ID_SEEKPOSITION			(0x53AC)
#define MKV_ID_SEEKENTRY			(0x4DBB)
#define MKV_ID_SEEKHEAD				(0x114D9B74)

// info
#define MKV_ID_TIMECODESCALE		(0x2AD7B1)
#define MKV_ID_DURATION				(0x4489)
#define MKV_ID_WRITINGAPP			(0x5741)
#define MKV_ID_MUXINGAPP			(0x4D80)

// track
#define MKV_ID_VIDEOPIXELWIDTH		(0xB0)
#define MKV_ID_VIDEOPIXELHEIGHT		(0xBA)

#define MKV_ID_AUDIOSAMPLINGFREQ	(0xB5)
#define MKV_ID_AUDIOBITDEPTH		(0x6264)
#define MKV_ID_AUDIOCHANNELS		(0x9F)

#define MKV_ID_TRACKNUMBER			(0xD7)
#define MKV_ID_TRACKUID				(0x73C5)
#define MKV_ID_TRACKTYPE			(0x83)
#define MKV_ID_TRACKVIDEO			(0xE0)
#define MKV_ID_TRACKAUDIO			(0xE1)
#define MKV_ID_TRACKCODECID			(0x86)
#define MKV_ID_TRACKCODECPRIVATE	(0x63A2)
#define MKV_ID_TRACKDEFAULTDURATION	(0x23E383)
#define MKV_ID_TRACKENTRY			(0xAE)
#define MKV_ID_TRACKCODECDELAY		(0x56AA)
#define MKV_ID_TRACKLANGUAGE		(0x22B59C)
#define MKV_ID_TRACKNAME			(0x536E)

// content encodings
#define MKV_ID_CONTENTENCODINGS			(0x6D80)
#define MKV_ID_CONTENTENCODING			(0x6240)
#define MKV_ID_CONTENTENCODINGORDER		(0x5031)
#define MKV_ID_CONTENTENCODINGSCOPE		(0x5032)
#define MKV_ID_CONTENTENCODINGTYPE		(0x5033)
#define MKV_ID_CONTENTENCRYPTION		(0x5035)
#define MKV_ID_CONTENTENCALGO			(0x47E1)
#define MKV_ID_CONTENTENCKEYID			(0x47E2)
#define MKV_ID_CONTENTENCAESSETTINGS	(0x47E7)
#define MKV_ID_AESSETTINGSCIPHERMODE	(0x47E8)

// index
#define MKV_ID_CUETIME				(0xB3)
#define MKV_ID_CUETRACKPOSITION		(0xB7)
#define MKV_ID_CUETRACK				(0xF7)
#define MKV_ID_CUECLUSTERPOSITION	(0xF1)
#define MKV_ID_CUERELATIVEPOSITION	(0xF0)
#define MKV_ID_POINTENTRY			(0xBB)

// cluster
#define MKV_ID_CLUSTERTIMECODE		(0xE7)
#define MKV_ID_SIMPLEBLOCK			(0xA3)
#define MKV_ID_BLOCKGROUP			(0xA0)
#define MKV_ID_BLOCK				(0xA1)
#define MKV_ID_REFERENCEBLOCK		(0xFB)
#define MKV_ID_CLUSTER				(0x1F43B675)

// sections
#define MKV_ID_SEGMENT				(0x18538067)
#define MKV_ID_INFO					(0x1549A966)
#define MKV_ID_TRACKS				(0x1654AE6B)
#define MKV_ID_CUES					(0x1C53BB6B)

#define MKV_MAX_CODEC_SIZE (50)		// must be greater than all mkv_codec_types values

// enums
enum {
	MKV_TRACK_TYPE_VIDEO = 0x1,
	MKV_TRACK_TYPE_AUDIO = 0x2,
};

// typedefs
typedef struct {
	vod_str_t mkv_codec_id;
	uint32_t codec_id;
	uint32_t format;
	bool_t extra_data_required;
} mkv_codec_type_t;

// constants
extern mkv_codec_type_t mkv_codec_types[];

#endif // __MKV_DEFS_H__
