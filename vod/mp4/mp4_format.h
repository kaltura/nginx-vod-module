#ifndef __MP4_FORMAT_H__
#define __MP4_FORMAT_H__

// includes
#include "../media_format.h"

// enums
enum {
	MP4_METADATA_PART_FTYP,
	MP4_METADATA_PART_MOOV,
	MP4_METADATA_PART_COUNT
};

// globals
extern media_format_t mp4_format;

#endif //__MP4_FORMAT_H__
