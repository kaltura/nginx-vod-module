#ifndef __HDS_AMF0_ENCODER_H__
#define __HDS_AMF0_ENCODER_H__

// includes
#include "../media_set.h"

// codecs
#define CODEC_ID_AVC 		(0x7)
#define SOUND_FORMAT_MP3 	(0x2)
#define SOUND_FORMAT_AAC 	(0xA)

// buffer size limit
#define AMF0_NUMBER sizeof(u_char) + sizeof(double)
#define AMF0_BOOLEAN sizeof(u_char) + sizeof(u_char)

#define AMF0_FIELD(group, name, type) sizeof(uint16_t) + sizeof(#name) - 1 + type +

static const uint32_t amf0_max_total_size =
	sizeof(u_char) + sizeof(uint16_t) + sizeof("onMetaData") - 1 +		// on metadata string
	sizeof(u_char) + sizeof(uint32_t) +									// array header
#include "hds_amf0_fields_x.h"
	sizeof(uint16_t) + sizeof(u_char);									// array footer

#undef AMF0_FIELD

// functions
u_char* hds_amf0_write_base64_metadata(u_char* p, u_char* temp_buffer, media_set_t* media_set, media_track_t** tracks);

#endif //__HDS_AMF0_ENCODER_H__
