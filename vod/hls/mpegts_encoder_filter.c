#include "mpegts_encoder_filter.h"
#include "bit_fields.h"
#include "../common.h"

#define THIS_FILTER (MEDIA_FILTER_MPEGTS)

#define member_size(type, member) sizeof(((type *)0)->member)
#define get_context(ctx) ((mpegts_encoder_state_t*)ctx->context[THIS_FILTER])

#define PCR_PID (0x100)
#define PRIVATE_STREAM_1_SID (0xBD)
#define FIRST_AUDIO_SID (0xC0)
#define FIRST_VIDEO_SID (0xE0)

#define SIZEOF_MPEGTS_HEADER (4)
#define MPEGTS_PACKET_USABLE_SIZE (MPEGTS_PACKET_SIZE - SIZEOF_MPEGTS_HEADER)
#define SIZEOF_MPEGTS_ADAPTATION_FIELD (2)
#define SIZEOF_PCR (6)
#define PMT_LENGTH_END_OFFSET (4)
#define SIZEOF_PES_HEADER (6)
#define SIZEOF_PES_OPTIONAL_HEADER (3)
#define SIZEOF_PES_PTS (5)

#define NO_TIMESTAMP ((uint64_t)-1)

#ifndef FF_PROFILE_AAC_HE
#define FF_PROFILE_AAC_HE   (4)
#endif

#ifndef FF_PROFILE_AAC_HE_V2
#define FF_PROFILE_AAC_HE_V2 (28)
#endif

#define SAMPLE_AES_AC3_EXTRA_DATA_SIZE (10)

// sample aes structs
typedef struct {
	u_char descriptor_tag[1];
	u_char descriptor_length[1];
	u_char format_identifier[4];
} registration_descriptor_t;

typedef struct {
	u_char audio_type[4];
	u_char priming[2];
	u_char version[1];
	u_char setup_data_length[1];
} audio_setup_information_t;

static const u_char pat_packet[] = {

	/* TS */
	0x47, 0x40, 0x00, 0x10, 0x00,
	/* PSI */
	0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
	/* PAT */
	0x00, 0x01, 0xef, 0xff,
	/* CRC */
	0x36, 0x90, 0xe2, 0x3d,
};

static const u_char pmt_header_template[] = {
	/* TS */
	0x47, 0x4f, 0xff, 0x10, 
	/* PSI */
	0x00, 0x02, 0xb0, 0x00, 0x00, 0x01, 0xc1, 0x00, 0x00,
	/* PMT */
	0xe1, 0x00, 0xf0, 0x11,
	/* Program descriptors */
	0x25, 0x0f, 0xff, 0xff, 0x49, 0x44, 0x33, 0x20, 0xff, 0x49,
	0x44, 0x33, 0x20, 0x00, 0x1f, 0x00, 0x01,
};

static const u_char pmt_entry_template_hevc[] = {
	0x06, 0xe0, 0x00, 0xf0, 0x06,
	0x05, 0x04, 0x48, 0x45, 0x56, 0x43		// registration_descriptor('HEVC')
};

static const u_char pmt_entry_template_avc[] = {
	0x1b, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_aac[] = {
	0x0f, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_mp3[] = {
	0x03, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_dts[] = {
	0x82, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_ac3[] = {
	0x81, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_sample_aes_avc[] = {
	0xdb, 0xe0, 0x00, 0xf0, 0x06,
	0x0f, 0x04, 0x7a, 0x61, 0x76, 0x63		// private_data_indicator_descriptor('zavc')
};

static const u_char pmt_entry_template_sample_aes_aac[] = {
	0xcf, 0xe1, 0x00, 0xf0, 0x00,
	0x0f, 0x04, 0x61, 0x61, 0x63, 0x64		// private_data_indicator_descriptor('aacd')
};

static const u_char pmt_entry_template_sample_aes_ac3[] = {
	0xc1, 0xe1, 0x00, 0xf0, 0x00,
	0x0f, 0x04, 0x61, 0x63, 0x33, 0x64		// private_data_indicator_descriptor('ac3d')
};

static const u_char pmt_entry_template_sample_aes_eac3[] = {
	0xc2, 0xe1, 0x00, 0xf0, 0x00,
	0x0f, 0x04, 0x65, 0x63, 0x33, 0x64		// private_data_indicator_descriptor('ec3d')
};

static const u_char pmt_entry_template_id3[] = {
	0x15, 0xe0, 0x00, 0xf0, 0x0f, 0x26, 0x0d, 0xff, 0xff, 0x49,
	0x44, 0x33, 0x20, 0xff, 0x49, 0x44, 0x33, 0x20, 0x00, 0x0f,
};

// Note: according to the sample-aes spec, this should be the first 10 bytes of the audio data
//		in practice, sending only the ac-3 syncframe magic is good enough (without the magic it doesn't play)
static u_char ac3_extra_data[SAMPLE_AES_AC3_EXTRA_DATA_SIZE] = {
	0x0b, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// from ffmpeg
static const uint32_t crc_table[256] = {
	0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9, 0x130476dc, 0x17c56b6b,
	0x1a864db2, 0x1e475005, 0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
	0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd, 0x4c11db70, 0x48d0c6c7,
	0x4593e01e, 0x4152fda9, 0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
	0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011, 0x791d4014, 0x7ddc5da3,
	0x709f7b7a, 0x745e66cd, 0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
	0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5, 0xbe2b5b58, 0xbaea46ef,
	0xb7a96036, 0xb3687d81, 0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
	0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49, 0xc7361b4c, 0xc3f706fb,
	0xceb42022, 0xca753d95, 0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
	0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d, 0x34867077, 0x30476dc0,
	0x3d044b19, 0x39c556ae, 0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
	0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16, 0x018aeb13, 0x054bf6a4,
	0x0808d07d, 0x0cc9cdca, 0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
	0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02, 0x5e9f46bf, 0x5a5e5b08,
	0x571d7dd1, 0x53dc6066, 0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
	0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e, 0xbfa1b04b, 0xbb60adfc,
	0xb6238b25, 0xb2e29692, 0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
	0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a, 0xe0b41de7, 0xe4750050,
	0xe9362689, 0xedf73b3e, 0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
	0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686, 0xd5b88683, 0xd1799b34,
	0xdc3abded, 0xd8fba05a, 0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
	0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb, 0x4f040d56, 0x4bc510e1,
	0x46863638, 0x42472b8f, 0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
	0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47, 0x36194d42, 0x32d850f5,
	0x3f9b762c, 0x3b5a6b9b, 0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
	0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623, 0xf12f560e, 0xf5ee4bb9,
	0xf8ad6d60, 0xfc6c70d7, 0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
	0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f, 0xc423cd6a, 0xc0e2d0dd,
	0xcda1f604, 0xc960ebb3, 0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
	0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b, 0x9b3660c6, 0x9ff77d71,
	0x92b45ba8, 0x9675461f, 0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
	0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640, 0x4e8ee645, 0x4a4ffbf2,
	0x470cdd2b, 0x43cdc09c, 0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
	0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24, 0x119b4be9, 0x155a565e,
	0x18197087, 0x1cd86d30, 0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
	0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088, 0x2497d08d, 0x2056cd3a,
	0x2d15ebe3, 0x29d4f654, 0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
	0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c, 0xe3a1cbc1, 0xe760d676,
	0xea23f0af, 0xeee2ed18, 0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
	0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0, 0x9abc8bd5, 0x9e7d9662,
	0x933eb0bb, 0x97ffad0c, 0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
	0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

static uint32_t 
mpegts_crc32(const u_char* data, int len)
{
	const u_char* end = data + len;
	uint32_t crc = 0xffffffff;

	for (; data < end; data++)
	{
		crc = (crc << 8) ^ crc_table[((crc >> 24) ^ *data) & 0xff];
	}

	return crc;
}

// stateless writing functions - copied from ngx_hls_mpegts with some refactoring
static u_char *
mpegts_write_pcr(u_char *p, uint64_t pcr)
{
	*p++ = (u_char) (pcr >> 25);
	*p++ = (u_char) (pcr >> 17);
	*p++ = (u_char) (pcr >> 9);
	*p++ = (u_char) (pcr >> 1);
	*p++ = (u_char) (pcr << 7 | 0x7e);
	*p++ = 0;
	
	return p;
}

static u_char *
mpegts_write_pts(u_char *p, unsigned fb, uint64_t pts)
{
	unsigned val;

	val = fb << 4 | (((pts >> 30) & 0x07) << 1) | 1;
	*p++ = (u_char) val;

	val = (((pts >> 15) & 0x7fff) << 1) | 1;
	*p++ = (u_char) (val >> 8);
	*p++ = (u_char) val;

	val = (((pts) & 0x7fff) << 1) | 1;
	*p++ = (u_char) (val >> 8);
	*p++ = (u_char) val;

	return p;
}

static vod_inline u_char *
mpegts_write_packet_header(u_char *p, unsigned pid, unsigned cc)
{	
	*p++ = 0x47;
	*p++ = (u_char) (pid >> 8);
	*p++ = (u_char) pid;
	*p++ = 0x10 | (cc & 0x0f); /* payload */
	
	return p;
}

static size_t
mpegts_get_pes_header_size(mpegts_stream_info_t* stream_info)
{
	size_t result;
	bool_t write_dts = stream_info->media_type == MEDIA_TYPE_VIDEO;

	result = SIZEOF_PES_HEADER + SIZEOF_PES_OPTIONAL_HEADER + SIZEOF_PES_PTS;

	if (stream_info->pid == PCR_PID)
	{
		result += SIZEOF_MPEGTS_ADAPTATION_FIELD + SIZEOF_PCR;
	}
	if (write_dts)
	{
		result += SIZEOF_PES_PTS;
	}

	return result;
}

static u_char *
mpegts_write_pes_header(
	u_char* cur_packet_start, 
	mpegts_stream_info_t* stream_info, 
	output_frame_t* f, 
	u_char** pes_size_ptr, 
	bool_t data_aligned)
{
	unsigned header_size;
	unsigned flags;
	u_char* p = cur_packet_start + SIZEOF_MPEGTS_HEADER;
	bool_t write_dts = stream_info->media_type == MEDIA_TYPE_VIDEO;

	cur_packet_start[1] |= 0x40; /* payload start indicator */

	if (stream_info->pid == PCR_PID)
	{
		cur_packet_start[3] |= 0x20; /* adaptation */

		*p++ = 1 + SIZEOF_PCR;	/* size */
		*p++ = 0x10; /* PCR */
		
		p = mpegts_write_pcr(p, f->dts + INITIAL_PCR);
	}

	/* PES header */

	*p++ = 0x00;
	*p++ = 0x00;
	*p++ = 0x01;
	*p++ = (u_char) stream_info->sid;

	header_size = SIZEOF_PES_PTS;
	flags = 0x80; /* PTS */

	if (write_dts) 
	{
		header_size += SIZEOF_PES_PTS;
		flags |= 0x40; /* DTS */
	}

	*pes_size_ptr = p;
	p += 2;		// skip pes_size, updated later
	*p++ = data_aligned ? 0x84 : 0x80; /* H222 */
	*p++ = (u_char) flags;
	*p++ = (u_char) header_size;

	p = mpegts_write_pts(p, flags >> 6, f->pts + INITIAL_DTS);

	if (write_dts) 
	{
		p = mpegts_write_pts(p, 1, f->dts + INITIAL_DTS);
	}
	
	return p;
}

static void
mpegts_add_stuffing(u_char* packet, u_char* p, unsigned stuff_size)
{
	u_char  *base;

	if (packet[3] & 0x20) 
	{
		/* has adaptation */
		base = &packet[5] + packet[4];
		vod_memmove(base + stuff_size, base, p - base);
		vod_memset(base, 0xff, stuff_size);
		packet[4] += (u_char) stuff_size;
	}
	else
	{
		/* no adaptation */
		packet[3] |= 0x20;
		vod_memmove(&packet[4] + stuff_size, &packet[4], p - &packet[4]);

		packet[4] = (u_char) (stuff_size - 1);
		if (stuff_size >= 2) 
		{
			packet[5] = 0;
			vod_memset(&packet[6], 0xff, stuff_size - 2);
		}
	}
}

static void
mpegts_copy_and_stuff(u_char* dest_packet, u_char* src_packet, u_char* src_pos, unsigned stuff_size)
{
	u_char  *base;
	u_char* p;

	if (src_packet[3] & 0x20)
	{
		/* has adaptation */
		base = &src_packet[5] + src_packet[4];

		p = vod_copy(dest_packet, src_packet, base - src_packet);
		dest_packet[4] += (u_char)stuff_size;

		vod_memset(p, 0xff, stuff_size);
		p += stuff_size;
	}
	else
	{
		/* no adaptation */
		base = &src_packet[4];

		p = vod_copy(dest_packet, src_packet, 4);
		dest_packet[3] |= 0x20;

		*p++ = (u_char)(stuff_size - 1);
		if (stuff_size >= 2)
		{
			*p++ = 0;
			vod_memset(p, 0xff, stuff_size - 2);
			p += stuff_size - 2;
		}
	}

	vod_memcpy(p, base, src_pos - base);
}

////////////////////////////////////

// PAT/PMT write functions
vod_status_t 
mpegts_encoder_init_streams(
	request_context_t* request_context, 
	hls_encryption_params_t* encryption_params,
	mpegts_encoder_init_streams_state_t* stream_state, 
	uint32_t segment_index)
{
	u_char* cur_packet;

	stream_state->request_context = request_context;
	stream_state->encryption_params = encryption_params;
	stream_state->segment_index = segment_index;
	stream_state->cur_pid = PCR_PID;
	stream_state->cur_video_sid = FIRST_VIDEO_SID;
	stream_state->cur_audio_sid = FIRST_AUDIO_SID;

	if (request_context->simulation_only)
	{
		stream_state->pmt_packet_start = NULL;
		return VOD_OK;
	}

	// append PAT packet
	cur_packet = vod_alloc(request_context->pool, MPEGTS_PACKET_SIZE * 2);
	if (cur_packet == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mpegts_encoder_init_streams: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	stream_state->pat_packet_start = cur_packet;

	vod_memcpy(cur_packet, pat_packet, sizeof(pat_packet));
	vod_memset(cur_packet + sizeof(pat_packet), 0xff, MPEGTS_PACKET_SIZE - sizeof(pat_packet));

	// make sure the continuity counters of the PAT/PMT are continous between segments
	cur_packet[3] |= (segment_index & 0x0F);

	// append PMT packet
	cur_packet += MPEGTS_PACKET_SIZE;
	stream_state->pmt_packet_start = cur_packet;
	stream_state->pmt_packet_end = cur_packet + MPEGTS_PACKET_SIZE;

	vod_memcpy(cur_packet, pmt_header_template, sizeof(pmt_header_template));
	cur_packet[3] |= (segment_index & 0x0F);
	stream_state->pmt_packet_pos = cur_packet + sizeof(pmt_header_template);

	return VOD_OK;
}

static void
mpegts_encoder_write_sample_aes_audio_pmt_entry(
	request_context_t* request_context,
	u_char* start,
	int entry_size,
	media_info_t* media_info)
{
	vod_str_t extra_data;
	u_char* p;

	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_AC3:
		extra_data.data = ac3_extra_data;
		extra_data.len = sizeof(ac3_extra_data);
		p = vod_copy(
			start,
			pmt_entry_template_sample_aes_ac3,
			sizeof(pmt_entry_template_sample_aes_ac3));
		break;

	case VOD_CODEC_ID_EAC3:
		extra_data = media_info->extra_data;
		p = vod_copy(
			start,
			pmt_entry_template_sample_aes_eac3,
			sizeof(pmt_entry_template_sample_aes_eac3));
		break;

	default:
		extra_data = media_info->extra_data;
		p = vod_copy(
			start, 
			pmt_entry_template_sample_aes_aac, 
			sizeof(pmt_entry_template_sample_aes_aac));
		break;
	}
	pmt_entry_set_es_info_length(start, entry_size - sizeof_pmt_entry);

	// registration_descriptor
	*p++ = 0x05;		// descriptor tag
	*p++ =				// descriptor length
		member_size(registration_descriptor_t, format_identifier) +
		sizeof(audio_setup_information_t) +
		extra_data.len;
	*p++ = 'a';		*p++ = 'p';		*p++ = 'a';		*p++ = 'd';			// apad

	// audio_setup_information
	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_AC3:
		*p++ = 'z';		*p++ = 'a';		*p++ = 'c';		*p++ = '3';			// zac3
		break;

	case VOD_CODEC_ID_EAC3:
		*p++ = 'z';		*p++ = 'e';		*p++ = 'c';		*p++ = '3';			// zec3
		break;

	default:
		switch (media_info->u.audio.codec_config.object_type - 1)
		{
		case FF_PROFILE_AAC_HE:
			*p++ = 'z';		*p++ = 'a';		*p++ = 'c';		*p++ = 'h';			// zach
			break;

		case FF_PROFILE_AAC_HE_V2:
			*p++ = 'z';		*p++ = 'a';		*p++ = 'c';		*p++ = 'p';			// zacp
			break;

		default:
			*p++ = 'z';		*p++ = 'a';		*p++ = 'a';		*p++ = 'c';			// zaac
			break;
		}
		break;
	}

	*p++ = 0;	*p++ = 0;		// priming
	*p++ = 1;					// version
	*p++ = extra_data.len;
	vod_memcpy(p, extra_data.data, extra_data.len);
}

static vod_status_t 
mpegts_encoder_add_stream(
	mpegts_encoder_init_streams_state_t* stream_state, 
	media_track_t* track,
	mpegts_stream_info_t* stream_info)
{
	const u_char* pmt_entry;
	int pmt_entry_size;

	stream_info->pid = stream_state->cur_pid++;

	if (stream_state->pmt_packet_start == NULL)			// simulation only
	{
		return VOD_OK;
	}

	switch (stream_info->media_type)
	{
	case MEDIA_TYPE_VIDEO:
		stream_info->sid = stream_state->cur_video_sid++;
		if (stream_state->encryption_params->type == HLS_ENC_SAMPLE_AES)
		{
			pmt_entry = pmt_entry_template_sample_aes_avc;
			pmt_entry_size = sizeof(pmt_entry_template_sample_aes_avc);
		}
		else
		{
			switch (track->media_info.codec_id)
			{
			case VOD_CODEC_ID_HEVC:
				pmt_entry = pmt_entry_template_hevc;
				pmt_entry_size = sizeof(pmt_entry_template_hevc);
				break;

			default:
				pmt_entry = pmt_entry_template_avc;
				pmt_entry_size = sizeof(pmt_entry_template_avc);
				break;
			}
		}
		break;

	case MEDIA_TYPE_AUDIO:
		stream_info->sid = stream_state->cur_audio_sid++;
		if (stream_state->encryption_params->type == HLS_ENC_SAMPLE_AES)
		{
			switch (track->media_info.codec_id)
			{
			case VOD_CODEC_ID_AC3:
				pmt_entry = pmt_entry_template_sample_aes_ac3;
				pmt_entry_size = sizeof(pmt_entry_template_sample_aes_ac3) +
					sizeof(registration_descriptor_t) +
					sizeof(audio_setup_information_t) +
					SAMPLE_AES_AC3_EXTRA_DATA_SIZE;
				break;

			case VOD_CODEC_ID_EAC3:
				pmt_entry = pmt_entry_template_sample_aes_eac3;
				pmt_entry_size = sizeof(pmt_entry_template_sample_aes_eac3) +
					sizeof(registration_descriptor_t) +
					sizeof(audio_setup_information_t) +
					track->media_info.extra_data.len;
				break;

			default:
				pmt_entry = pmt_entry_template_sample_aes_aac;
				pmt_entry_size = sizeof(pmt_entry_template_sample_aes_aac) +
					sizeof(registration_descriptor_t) +
					sizeof(audio_setup_information_t) +
					track->media_info.extra_data.len;
				break;
			}
		}
		else
		{
			switch (track->media_info.codec_id)
			{
			case VOD_CODEC_ID_MP3:
				pmt_entry = pmt_entry_template_mp3;
				pmt_entry_size = sizeof(pmt_entry_template_mp3);
				break;

			case VOD_CODEC_ID_DTS:
				pmt_entry = pmt_entry_template_dts;
				pmt_entry_size = sizeof(pmt_entry_template_dts);
				break;

			case VOD_CODEC_ID_AC3:
			case VOD_CODEC_ID_EAC3:
				pmt_entry = pmt_entry_template_ac3;
				pmt_entry_size = sizeof(pmt_entry_template_ac3);
				break;

			default:
				pmt_entry = pmt_entry_template_aac;
				pmt_entry_size = sizeof(pmt_entry_template_aac);
				break;
			}
		}
		break;

	case MEDIA_TYPE_NONE:
		stream_info->sid = PRIVATE_STREAM_1_SID;
		pmt_entry = pmt_entry_template_id3;
		pmt_entry_size = sizeof(pmt_entry_template_id3);
		break;

	default:
		vod_log_error(VOD_LOG_ERR, stream_state->request_context->log, 0,
			"mpegts_encoder_add_stream: invalid media type %d", stream_info->media_type);
		return VOD_BAD_REQUEST;
	}

	if (stream_state->pmt_packet_pos + pmt_entry_size + sizeof(uint32_t) >= 
		stream_state->pmt_packet_end)
	{
		vod_log_error(VOD_LOG_ERR, stream_state->request_context->log, 0,
			"mpegts_encoder_add_stream: stream definitions overflow PMT size");
		return VOD_BAD_DATA;
	}

	if (stream_info->media_type == MEDIA_TYPE_AUDIO &&
		stream_state->encryption_params->type == HLS_ENC_SAMPLE_AES)
	{
		mpegts_encoder_write_sample_aes_audio_pmt_entry(
			stream_state->request_context,
			stream_state->pmt_packet_pos,
			pmt_entry_size,
			&track->media_info);
	}
	else
	{
		vod_memcpy(stream_state->pmt_packet_pos, pmt_entry, pmt_entry_size);
	}
	pmt_entry_set_elementary_pid(stream_state->pmt_packet_pos, stream_info->pid);
	stream_state->pmt_packet_pos += pmt_entry_size;
	return VOD_OK;
}

void 
mpegts_encoder_finalize_streams(mpegts_encoder_init_streams_state_t* stream_state, vod_str_t* ts_header)
{
	u_char* p = stream_state->pmt_packet_pos;
	u_char* crc_start_offset;
	uint32_t crc;

	if (stream_state->pmt_packet_start == NULL)			// simulation only
	{
		return;
	}

	// update the length in the PMT header
	pmt_set_section_length(stream_state->pmt_packet_start + SIZEOF_MPEGTS_HEADER, 
		p - (stream_state->pmt_packet_start + SIZEOF_MPEGTS_HEADER + PMT_LENGTH_END_OFFSET) + sizeof(crc));

	// append the CRC
	crc_start_offset = stream_state->pmt_packet_start + SIZEOF_MPEGTS_HEADER + 1;	// the PMT pointer field is not part of the CRC
	crc = mpegts_crc32(crc_start_offset, p - crc_start_offset);
	*p++ = (u_char)(crc >> 24);
	*p++ = (u_char)(crc >> 16);
	*p++ = (u_char)(crc >> 8);
	*p++ = (u_char)(crc);

	// set the padding
	vod_memset(p, 0xFF, stream_state->pmt_packet_end - p);

	ts_header->data = stream_state->pat_packet_start;
	ts_header->len = MPEGTS_PACKET_SIZE * 2;
}

// stateful functions
static vod_inline vod_status_t 
mpegts_encoder_init_packet(mpegts_encoder_state_t* state, bool_t write_direct)
{
	if (write_direct || !state->interleave_frames)
	{
		state->last_queue_offset = state->queue->cur_offset;

		state->cur_packet_start = write_buffer_queue_get_buffer(state->queue, MPEGTS_PACKET_SIZE, state);
		if (state->cur_packet_start == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mpegts_encoder_init_packet: write_buffer_queue_get_buffer failed");
			return VOD_ALLOC_FAILED;
		}
	}
	else
	{
		state->cur_packet_start = state->temp_packet;
	}

	state->last_frame_pts = NO_TIMESTAMP;
	state->cur_packet_end = state->cur_packet_start + MPEGTS_PACKET_SIZE;
	state->cur_pos = mpegts_write_packet_header(state->cur_packet_start, state->stream_info.pid, state->cc);
	state->cc++;

	return VOD_OK;
}

static vod_status_t
mpegts_encoder_stuff_cur_packet(mpegts_encoder_state_t* state)
{
	unsigned stuff_size = state->cur_packet_end - state->cur_pos;
	unsigned pes_size;
	u_char* cur_packet;

	if (state->pes_bytes_written != 0 &&
		state->stream_info.media_type != MEDIA_TYPE_VIDEO)
	{
		// the trailing part of the last pes was not counted in its size, add it now
		pes_size = ((uint16_t)(state->cur_pes_size_ptr[0]) << 8) | state->cur_pes_size_ptr[1];
		pes_size += state->pes_bytes_written;
		if (pes_size > 0xffff)
		{
			pes_size = 0;
		}
		state->cur_pes_size_ptr[0] = (u_char)(pes_size >> 8);
		state->cur_pes_size_ptr[1] = (u_char)pes_size;

		state->pes_bytes_written = 0;
	}

	if (state->cur_packet_start == state->temp_packet &&
		state->interleave_frames)
	{
		// allocate a packet from the queue
		state->last_queue_offset = state->queue->cur_offset;

		cur_packet = write_buffer_queue_get_buffer(state->queue, MPEGTS_PACKET_SIZE, state);
		if (cur_packet == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mpegts_encoder_stuff_cur_packet: write_buffer_queue_get_buffer failed");
			return VOD_ALLOC_FAILED;
		}

		state->cur_packet_start = NULL;

		// copy the temp packet and stuff it
		if (stuff_size > 0)
		{
			mpegts_copy_and_stuff(cur_packet, state->temp_packet, state->cur_pos, stuff_size);
		}
		else
		{
			vod_memcpy(cur_packet, state->temp_packet, MPEGTS_PACKET_SIZE);
		}
	}
	else
	{
		// stuff the current packet in place
		if (stuff_size > 0)
		{
			mpegts_add_stuffing(state->cur_packet_start, state->cur_pos, stuff_size);
		}
	}

	state->cur_pos = state->cur_packet_end;
	state->send_queue_offset = VOD_MAX_OFF_T_VALUE;

	return VOD_OK;
}

static vod_status_t 
mpegts_encoder_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	mpegts_encoder_state_t* state = get_context(context);
	mpegts_encoder_state_t* last_writer_state;
	vod_status_t rc;
	size_t pes_header_size;
	u_char* excess_pos;
	u_char* pes_packet_start;
	u_char* pes_start;
	u_char* p;
	size_t excess_size;
	bool_t write_direct;

	last_writer_state = state->queue->last_writer_context;
	if (!state->interleave_frames && last_writer_state != state && last_writer_state != NULL)
	{
		// frame interleaving is disabled and the last packet that was written belongs to a different stream, close it
		rc = mpegts_encoder_stuff_cur_packet(last_writer_state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	state->flushed_frame_bytes = 0;
	state->header_size = frame->header_size;

	state->send_queue_offset = state->last_queue_offset;

	pes_header_size = mpegts_get_pes_header_size(&state->stream_info);

	if (state->cur_pos >= state->cur_packet_end)
	{
		// current packet is full, start a new packet
		write_direct = pes_header_size + frame->size >= MPEGTS_PACKET_USABLE_SIZE;

		rc = mpegts_encoder_init_packet(state, write_direct);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->cur_pos = mpegts_write_pes_header(state->cur_packet_start, &state->stream_info, frame, &state->cur_pes_size_ptr, TRUE);

		state->packet_bytes_left = state->cur_packet_end - state->cur_pos;

		return VOD_OK;
	}

	if (state->last_frame_pts != NO_TIMESTAMP)
	{
		frame->pts = state->last_frame_pts;
	}

	if (state->cur_pos + pes_header_size < state->cur_packet_end)
	{
		// current packet has enough room to push the pes without getting full
		pes_packet_start = state->cur_packet_start;
		pes_start = pes_packet_start + SIZEOF_MPEGTS_HEADER;

		vod_memmove(
			pes_start + pes_header_size,
			pes_start,
			state->cur_pos - pes_start);

		state->cur_pos += pes_header_size;

		// write the pes
		mpegts_write_pes_header(pes_packet_start, &state->stream_info, frame, &state->cur_pes_size_ptr, FALSE);

		state->packet_bytes_left = state->cur_packet_end - state->cur_pos;

		return VOD_OK;
	}

	// find the excess that has to be pushed to a new packet
	excess_size = state->cur_pos + pes_header_size - state->cur_packet_end;
	excess_pos = state->cur_pos - excess_size;

	if (state->cur_packet_start == state->temp_packet &&
		state->interleave_frames)
	{
		// allocate packet from the queue
		state->last_queue_offset = state->queue->cur_offset;

		pes_packet_start = write_buffer_queue_get_buffer(state->queue, MPEGTS_PACKET_SIZE, state);
		if (pes_packet_start == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mpegts_encoder_start_frame: write_buffer_queue_get_buffer failed");
			return VOD_ALLOC_FAILED;
		}

		// copy the packet and push in the pes
		vod_memcpy(pes_packet_start, state->temp_packet, SIZEOF_MPEGTS_HEADER);

		p = mpegts_write_pes_header(pes_packet_start, &state->stream_info, frame, &state->cur_pes_size_ptr, FALSE);

		vod_memcpy(
			p,
			state->temp_packet + SIZEOF_MPEGTS_HEADER,
			MPEGTS_PACKET_USABLE_SIZE - pes_header_size);

		pes_packet_start = NULL;
	}
	else
	{
		pes_packet_start = state->cur_packet_start;
	}

	if (excess_size > 0)
	{
		// copy the excess to a new packet
		write_direct = excess_size + frame->size >= MPEGTS_PACKET_USABLE_SIZE;

		rc = mpegts_encoder_init_packet(state, write_direct);
		if (rc != VOD_OK)
		{
			return rc;
		}

		vod_memmove(state->cur_pos, excess_pos, excess_size);
		state->cur_pos += excess_size;

		state->packet_bytes_left = state->cur_packet_end - state->cur_pos;
	}
	else
	{
		state->cur_pos = state->cur_packet_end;
		state->cur_packet_start = NULL;

		state->packet_bytes_left = MPEGTS_PACKET_USABLE_SIZE;
	}

	if (pes_packet_start != NULL)
	{
		// make room for the pes
		pes_start = pes_packet_start + SIZEOF_MPEGTS_HEADER;

		vod_memmove(
			pes_start + pes_header_size,
			pes_start,
			MPEGTS_PACKET_USABLE_SIZE - pes_header_size);

		// write the pes
		mpegts_write_pes_header(pes_packet_start, &state->stream_info, frame, &state->cur_pes_size_ptr, FALSE);
	}

	return VOD_OK;
}

static vod_status_t 
mpegts_encoder_write(media_filter_context_t* context, const u_char* buffer, uint32_t size)
{
	mpegts_encoder_state_t* state = get_context(context);
	uint32_t packet_used_size;
	uint32_t cur_size;
	uint32_t initial_size;
	u_char* cur_packet;
	vod_status_t rc;
	bool_t write_direct;

	state->pes_bytes_written += size;

	// make sure we have a packet
	if (state->cur_pos >= state->cur_packet_end)
	{
		write_direct = size >= MPEGTS_PACKET_USABLE_SIZE;

		rc = mpegts_encoder_init_packet(state, write_direct);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// if current packet has enough room for the whole buffer, just add it
	if (state->cur_pos + size < state->cur_packet_end)
	{
		state->cur_pos = vod_copy(state->cur_pos, buffer, size);
		return VOD_OK;
	}

	// fill the current packet
	cur_size = state->cur_packet_end - state->cur_pos;

	if (state->cur_packet_start == state->temp_packet &&
		state->interleave_frames)
	{
		// flush the temp packet
		state->last_queue_offset = state->queue->cur_offset;

		cur_packet = write_buffer_queue_get_buffer(state->queue, MPEGTS_PACKET_SIZE, state);
		if (cur_packet == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mpegts_encoder_write: write_buffer_queue_get_buffer failed");
			return VOD_ALLOC_FAILED;
		}

		state->cur_packet_start = NULL;

		// update the pes size ptr if needed
		if (state->cur_pes_size_ptr >= state->temp_packet && state->cur_pes_size_ptr < state->temp_packet + MPEGTS_PACKET_SIZE)
		{
			state->cur_pes_size_ptr = cur_packet + (state->cur_pes_size_ptr - state->temp_packet);
		}

		// write the packet
		packet_used_size = state->cur_pos - state->temp_packet;
		vod_memcpy(cur_packet, state->temp_packet, packet_used_size);
		vod_memcpy(cur_packet + packet_used_size, buffer, cur_size);
	}
	else
	{
		vod_memcpy(state->cur_pos, buffer, cur_size);
	}

	state->flushed_frame_bytes += state->packet_bytes_left;
	state->packet_bytes_left = MPEGTS_PACKET_USABLE_SIZE;

	buffer += cur_size;
	size -= cur_size;

	// write full packets
	initial_size = size;

	while (size >= MPEGTS_PACKET_USABLE_SIZE)
	{
		rc = mpegts_encoder_init_packet(state, TRUE);
		if (rc != VOD_OK)
		{
			return rc;
		}

		vod_memcpy(state->cur_pos, buffer, MPEGTS_PACKET_USABLE_SIZE);
		buffer += MPEGTS_PACKET_USABLE_SIZE;
		size -= MPEGTS_PACKET_USABLE_SIZE;
	}

	state->flushed_frame_bytes += initial_size - size;

	// write any residue
	if (size > 0)
	{
		rc = mpegts_encoder_init_packet(state, FALSE);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->cur_pos = vod_copy(state->cur_pos, buffer, size);
	}
	else
	{
		state->cur_pos = state->cur_packet_end;
	}

	return VOD_OK;
}

static vod_status_t 
mpegts_append_null_packet(mpegts_encoder_state_t* state)
{
	u_char* packet;
	vod_status_t rc;

	rc = mpegts_encoder_init_packet(state, TRUE);
	if (rc != VOD_OK)
	{
		return rc;
	}

	packet = state->cur_packet_start;
	packet[3] |= 0x20;
	packet[4] = (u_char)MPEGTS_PACKET_USABLE_SIZE - 1;
	packet[5] = 0;
	vod_memset(&packet[6], 0xff, MPEGTS_PACKET_USABLE_SIZE - 2);
	return VOD_OK;
}

static vod_status_t 
mpegts_encoder_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	mpegts_encoder_state_t* state = get_context(context);
	unsigned pes_size;
	vod_status_t rc;
	bool_t stuff_packet;

	stuff_packet = state->align_frames ||
		state->cur_pos >= state->cur_packet_end ||
		state->flushed_frame_bytes < state->header_size ||
		last_stream_frame;

	// update the size in the pes header
	if (state->stream_info.media_type == MEDIA_TYPE_VIDEO && !state->align_frames)
	{
		pes_size = 0;
	}
	else
	{
		pes_size = SIZEOF_PES_OPTIONAL_HEADER + SIZEOF_PES_PTS + state->pes_bytes_written;
		if (state->stream_info.media_type == MEDIA_TYPE_VIDEO)
		{
			pes_size += SIZEOF_PES_PTS;		// dts
		}

		if (pes_size > 0xffff)
		{
			pes_size = 0;
		}

		if (!stuff_packet)
		{
			// the last ts packet was not closed, its size should be counted in the next pes packet
			state->pes_bytes_written = state->cur_pos - state->cur_packet_start - SIZEOF_MPEGTS_HEADER;
			pes_size -= state->pes_bytes_written;
		}
		else
		{
			state->pes_bytes_written = 0;
		}
	}

	state->cur_pes_size_ptr[0] = (u_char)(pes_size >> 8);
	state->cur_pes_size_ptr[1] = (u_char)pes_size;

	// stuff the packet if needed and update the send offset
	if (stuff_packet)
	{
		rc = mpegts_encoder_stuff_cur_packet(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// on the last frame, add null packets to set the continuity counters
	if (last_stream_frame && 
		state->stream_info.media_type != MEDIA_TYPE_NONE)		// don't output null packets in id3
	{
		while (state->cc & 0x0F)
		{
			rc = mpegts_append_null_packet(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		state->cur_pos = state->cur_packet_end;
	}

	return VOD_OK;
}

vod_status_t
mpegts_encoder_start_sub_frame(media_filter_context_t* context, output_frame_t* frame)
{
	mpegts_encoder_state_t* state = get_context(context);
	vod_status_t rc;
	bool_t write_direct;

	if (state->cur_pos >= state->cur_packet_end)
	{
		write_direct = frame->size >= MPEGTS_PACKET_USABLE_SIZE;

		rc = mpegts_encoder_init_packet(state, write_direct);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->last_frame_pts = frame->pts;

		return VOD_OK;
	}

	if (state->last_frame_pts == NO_TIMESTAMP)
	{
		state->last_frame_pts = frame->pts;
	}

	return VOD_OK;
}


void 
mpegts_encoder_simulated_start_segment(write_buffer_queue_t* queue)
{
	queue->cur_offset = 2 * MPEGTS_PACKET_SIZE;		// PAT & PMT
	queue->last_writer_context = NULL;
}

static void
mpegts_encoder_simulated_stuff_cur_packet(mpegts_encoder_state_t* state)
{
	write_buffer_queue_t* queue = state->queue;

	if (state->cur_frame_start_pos == -1)
	{
		state->cur_frame_start_pos = queue->cur_offset;
	}

	if (state->temp_packet_size > 0)
	{
		queue->cur_offset += MPEGTS_PACKET_SIZE;
		queue->last_writer_context = state;
		state->cc++;
		state->temp_packet_size = 0;
	}

	if (state->last_frame_end_pos == -1)
	{
		state->last_frame_end_pos = queue->cur_offset;
	}
	state->cur_frame_end_pos = queue->cur_offset;
}

static void
mpegts_encoder_simulated_start_frame(media_filter_context_t* context, output_frame_t* frame)
{
	mpegts_encoder_state_t* state = get_context(context);
	write_buffer_queue_t* queue = state->queue;
	mpegts_encoder_state_t* last_writer_state = queue->last_writer_context;

	state->last_frame_start_pos = state->cur_frame_start_pos;
	state->last_frame_end_pos = state->cur_frame_end_pos;
	state->cur_frame_start_pos = -1;
	state->cur_frame_end_pos = -1;

	if (!state->interleave_frames &&
		last_writer_state != state &&
		last_writer_state != NULL &&
		last_writer_state->temp_packet_size > 0)
	{
		mpegts_encoder_simulated_stuff_cur_packet(last_writer_state);
	}

	state->flushed_frame_bytes = 0;
	state->header_size = frame->header_size;

	state->temp_packet_size += mpegts_get_pes_header_size(&state->stream_info);

	if (state->temp_packet_size >= MPEGTS_PACKET_USABLE_SIZE)
	{
		state->cur_frame_start_pos = queue->cur_offset;

		queue->cur_offset += MPEGTS_PACKET_SIZE;
		queue->last_writer_context = state;
		state->cc++;
		state->temp_packet_size -= MPEGTS_PACKET_USABLE_SIZE;

		if (state->temp_packet_size == 0)
		{
			state->last_frame_end_pos = queue->cur_offset;
		}
	}

	state->packet_bytes_left = MPEGTS_PACKET_USABLE_SIZE - state->temp_packet_size;
}

static void
mpegts_encoder_simulated_write(media_filter_context_t* context, uint32_t size)
{
	mpegts_encoder_state_t* state = get_context(context);
	write_buffer_queue_t* queue;
	uint32_t packet_count;

	size += state->temp_packet_size;

	packet_count = size / MPEGTS_PACKET_USABLE_SIZE;
	state->temp_packet_size = size - packet_count * MPEGTS_PACKET_USABLE_SIZE;

	if (packet_count <= 0)
	{
		return;
	}

	state->flushed_frame_bytes += state->packet_bytes_left + (packet_count - 1) * MPEGTS_PACKET_USABLE_SIZE;
	state->packet_bytes_left = MPEGTS_PACKET_USABLE_SIZE;

	queue = state->queue;

	if (state->cur_frame_start_pos == -1)
	{
		state->cur_frame_start_pos = queue->cur_offset;
	}

	if (state->last_frame_end_pos == -1)
	{
		state->last_frame_end_pos = queue->cur_offset + MPEGTS_PACKET_SIZE;
	}

	queue->cur_offset += packet_count * MPEGTS_PACKET_SIZE;
	queue->last_writer_context = state;
	state->cc += packet_count;
}

static void
mpegts_encoder_simulated_flush_frame(media_filter_context_t* context, bool_t last_stream_frame)
{
	mpegts_encoder_state_t* state = get_context(context);
	write_buffer_queue_t* queue = state->queue;

	if (state->align_frames ||
		state->temp_packet_size == 0 ||
		state->flushed_frame_bytes < state->header_size ||
		last_stream_frame)
	{
		mpegts_encoder_simulated_stuff_cur_packet(state);
	}

	// on the last frame, add null packets to set the continuity counters
	if (last_stream_frame)
	{
		if ((state->cc & 0x0F) != 0 &&
			state->stream_info.media_type != MEDIA_TYPE_NONE)	// don't output null packets in id3
		{
			queue->cur_offset += (0x10 - (state->cc & 0x0F)) * MPEGTS_PACKET_SIZE;
			queue->last_writer_context = state;
		}
		state->cc = state->initial_cc;
	}
}


static const media_filter_t mpegts_encoder = {
	mpegts_encoder_start_frame,
	mpegts_encoder_write,
	mpegts_encoder_flush_frame,
	mpegts_encoder_simulated_start_frame,
	mpegts_encoder_simulated_write,
	mpegts_encoder_simulated_flush_frame,
};

vod_status_t
mpegts_encoder_init(
	media_filter_t* filter,
	mpegts_encoder_state_t* state,
	mpegts_encoder_init_streams_state_t* stream_state,
	media_track_t* track,
	write_buffer_queue_t* queue,
	bool_t interleave_frames,
	bool_t align_frames)
{
	request_context_t* request_context = stream_state->request_context;
	vod_status_t rc;

	vod_memzero(state, sizeof(*state));
	state->request_context = request_context;
	state->queue = queue;
	state->interleave_frames = interleave_frames;
	state->align_frames = align_frames;

	if (track != NULL)
	{
		state->stream_info.media_type = track->media_info.media_type;
	}
	else
	{
		// id3 track
		state->stream_info.media_type = MEDIA_TYPE_NONE;
		state->initial_cc = state->cc = stream_state->segment_index & 0x0f;
	}

	rc = mpegts_encoder_add_stream(
		stream_state,
		track,
		&state->stream_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	*filter = mpegts_encoder;

	if (request_context->simulation_only || !interleave_frames)
	{
		return VOD_OK;
	}

	state->temp_packet = vod_alloc(request_context->pool, MPEGTS_PACKET_SIZE);
	if (state->temp_packet == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mpegts_encoder_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}
