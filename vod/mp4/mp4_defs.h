#ifndef __MP4_DEFS_H__
#define __MP4_DEFS_H__

#include "../common.h"

// constants
#define ATOM_HEADER_SIZE (8)
#define ATOM_HEADER64_SIZE (16)

// these constants can be generated with python - 'moov'[::-1].encode('hex')
#define ATOM_NAME_FTYP (0x70797466)		// file type
#define ATOM_NAME_MOOV (0x766f6f6d)		// movie header
#define ATOM_NAME_TRAK (0x6b617274)		// track header
#define ATOM_NAME_MDIA (0x6169646d)		// media
#define ATOM_NAME_HDLR (0x726c6468)		// handler type
#define ATOM_NAME_MDHD (0x6468646d)		// media header
#define ATOM_NAME_MINF (0x666e696d)		// media information
#define ATOM_NAME_STBL (0x6c627473)		// sample table
#define ATOM_NAME_STCO (0x6f637473)		// sample table chunk offset
#define ATOM_NAME_CO64 (0x34366f63)		// sample table chunk offset 64 bit
#define ATOM_NAME_STSC (0x63737473)		// sample table sample to chunk map
#define ATOM_NAME_STSZ (0x7a737473)		// sample table sizes
#define ATOM_NAME_STZ2 (0x327a7473)		// sample table sizes
#define ATOM_NAME_STTS (0x73747473)		// sample table time to sample map
#define ATOM_NAME_CTTS (0x73747463)		// composition time to sample
#define ATOM_NAME_STSS (0x73737473)		// sample table sync samples
#define ATOM_NAME_STSD (0x64737473)		// sample table sample description
#define ATOM_NAME_AVCC (0x43637661)		// advanced video codec configuration
#define ATOM_NAME_HVCC (0x43637668)		// high efficiency video codec configuration
#define ATOM_NAME_ESDS (0x73647365)		// elementary stream description
#define ATOM_NAME_WAVE (0x65766177)		// 
#define ATOM_NAME_DINF (0x666e6964)		// data information
#define ATOM_NAME_TKHD (0x64686b74)		// track header
#define ATOM_NAME_MVHD (0x6468766d)		// movie header
#define ATOM_NAME_VMHD (0x64686d76)		// video media header
#define ATOM_NAME_SMHD (0x64686d73)		// sound media header

#define ATOM_NAME_NULL (0x00000000)

#define HDLR_TYPE_VIDE (0x65646976)		// video track
#define HDLR_TYPE_SOUN (0x6e756f73)		// audio track

// MP4 constants from ffmpeg
#define MP4ODescrTag				0x01
#define MP4IODescrTag				0x02
#define MP4ESDescrTag				0x03
#define MP4DecConfigDescrTag		0x04
#define MP4DecSpecificDescrTag		0x05
#define MP4SLDescrTag				0x06

// mp4 atom structs
typedef struct {
	u_char    version[1];
	u_char    flags[3];
	u_char    creation_time[4];
	u_char    modification_time[4];
	u_char    timescale[4];
	u_char    duration[4];
	// Note: additional fields not listed
} mvhd_atom_t;

typedef struct {
	u_char    version[1];
	u_char    flags[3];
	u_char    creation_time[8];
	u_char    modification_time[8];
	u_char    timescale[4];
	u_char    duration[8];
	// Note: additional fields not listed
} mvhd64_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	creation_time[4];
	u_char	modification_time[4];
	u_char	track_id[4];
	u_char  reserved1[4];
	u_char  duration[4];
	// Note: additional fields not listed
} tkhd_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	creation_time[8];
	u_char	modification_time[8];
	u_char	track_id[4];
	u_char  reserved1[4];
	u_char  duration[8];
	// Note: additional fields not listed
} tkhd64_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stco_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stsc_atom_t;

typedef struct {
	u_char	first_chunk[4];
	u_char	samples_per_chunk[4];
	u_char	sample_desc[4];
} stsc_entry_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	uniform_size[4];
	u_char	entries[4];
} stsz_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	reserved[3];
	u_char	field_size[1];
	u_char	entries[4];
} stz2_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stts_atom_t;

typedef struct {
	u_char	count[4];
	u_char	duration[4];
} stts_entry_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} ctts_atom_t;

typedef struct {
	u_char	count[4];
	u_char	duration[4];
} ctts_entry_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stss_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	entries[4];
} stsd_atom_t;

typedef struct {
	u_char	size[4];
	u_char	format[4];
} stsd_entry_header_t;

typedef struct {
	u_char	reserved1[4];
	u_char	reserved2[2];
	u_char	dref_id[2];
} stsd_large_entry_header_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	creation_time[4];
	u_char	modification_time[4];
	u_char	timescale[4];
	u_char	duration[4];
	u_char	language[2];
	u_char	quality[2];
} mdhd_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	creation_time[8];
	u_char	modification_time[8];
	u_char	timescale[4];
	u_char	duration[8];
	u_char	language[2];
	u_char	quality[2];
} mdhd64_atom_t;

typedef struct {
	u_char	version[1];
	u_char	flags[3];
	u_char	ctype[4];
	u_char	type[4];
	u_char	component_manufacture[4];
	u_char	component_flags[4];
	u_char	component_flags_mask[4];
} hdlr_atom_t;

typedef struct {
	u_char	version[2];
	u_char	revision_level[2];
	u_char	vendor[4];
	u_char	temporal_quality[4];
	u_char	spatial_quality[4];
	u_char	width[2];
	u_char	height[2];
	u_char	horiz_resolution[4];
	u_char	vert_resolution[4];
	u_char	data_size[4];
	u_char	frames_per_samples[2];
	u_char	codec_name[32];
	u_char	bits_per_coded_sample[2];
	u_char	colortable_id[2];
} stsd_video_t;

typedef struct {
	u_char	version[2];
	u_char	revision_level[2];
	u_char	vendor[4];
	u_char	channels[2];
	u_char	bits_per_coded_sample[2];
	u_char	audio_cid[2];
	u_char	packet_size[2];
	u_char	sample_rate[4];
} stsd_audio_t;

typedef struct {
	u_char	samples_per_frame[4];
	u_char	bytes_per_packet[4];
	u_char	bytes_per_frame[4];
	u_char	bytes_per_sample[4];
} stsd_audio_qt_version_1_t;

typedef struct {
	u_char	sizeof_struct[4];
	u_char	sample_rate[8];
	u_char	channels[4];
	u_char	fixed[4];
	u_char	bits_per_sample[4];
	u_char	flags[4];
	u_char	bytes_per_frame[4];
	u_char	samples_per_frame[4];
} stsd_audio_qt_version_2_t;

typedef struct {
	u_char	color_start[4];
	u_char	color_count[2];
	u_char	color_end[2];
} stsd_video_palette_header_t;

typedef struct {
	u_char	a[2];
	u_char	r[2];
	u_char	g[2];
	u_char	b[2];
} stsd_video_palette_entry_t;

typedef struct {
	u_char object_type_id[1];
	u_char stream_type[1];
	u_char buffer_size[3];
	u_char max_bitrate[4];
	u_char avg_bitrate[4];
} config_descr_t;

#endif // __MP4_DEFS_H__
