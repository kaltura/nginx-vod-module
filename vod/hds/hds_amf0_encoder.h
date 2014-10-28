#ifndef __HDS_AMF0_ENCODER_H__
#define __HDS_AMF0_ENCODER_H__

// includes
#include "../mp4_parser.h"
#include "../common.h"

// codecs
#define CODEC_ID_AVC 		(0x7)
#define SOUND_FORMAT_AAC 	(0xA)

// buffer size limit
#define AMF0_NUMBER sizeof(u_char) + sizeof(double)
#define AMF0_BOOLEAN sizeof(u_char) + sizeof(u_char)

#define AMF0_FIELD(group, name, type) sizeof(uint16_t) + sizeof(#name) - 1 + type +

static const int amf0_max_total_size =
	sizeof(u_char)+sizeof(uint16_t)+sizeof("onMetaData") - 1 +		// on metadata string
	sizeof(u_char)+sizeof(uint32_t)+									// array header
#include "hds_amf0_fields_x.h"
	sizeof(uint16_t)+sizeof(u_char);									// array footer

#undef AMF0_FIELD

// functions
void hds_get_max_duration(mpeg_stream_metadata_t** streams, uint64_t* duration, uint32_t* timescale);

u_char* hds_amf0_write_base64_metadata(u_char* p, u_char* temp_buffer, mpeg_stream_metadata_t** streams);

#endif //__HDS_AMF0_ENCODER_H__
