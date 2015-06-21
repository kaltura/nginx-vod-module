#include "mpegts_encoder_filter.h"
#include "bit_fields.h"
#include "../mp4/mp4_parser.h"		// for MEDIA_TYPE_XXX
#include "../common.h"

#define PCR_PID (0x100)
#define FIRST_VIDEO_SID (0xE0)
#define FIRST_AUDIO_SID (0xC0)

#define SIZEOF_MPEGTS_HEADER (4)
#define SIZEOF_MPEGTS_ADAPTATION_FIELD (2)
#define SIZEOF_PCR (6)
#define PMT_LENGTH_END_OFFSET (4)
#define SIZEOF_PES_HEADER (6)
#define SIZEOF_PES_OPTIONAL_HEADER (3)
#define SIZEOF_PES_PTS (5)

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

static const u_char pmt_entry_template_h264[] = {
	0x1b, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_aac[] = {
	0x0f, 0xe0, 0x00, 0xf0, 0x00,
};

static const u_char pmt_entry_template_id3[] = {
	0x15, 0xe0, 0x00, 0xf0, 0x0f, 0x26, 0x0d, 0xff, 0xff, 0x49,
	0x44, 0x33, 0x20, 0xff, 0x49, 0x44, 0x33, 0x20, 0x00, 0x0f,
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

static u_char *
mpegts_write_packet_header(u_char *p, unsigned pid, unsigned cc, bool_t first)
{	
	*p++ = 0x47;
	*p++ = (u_char) (pid >> 8);

	if (first) 
	{
		p[-1] |= 0x40;
	}

	*p++ = (u_char) pid;
	*p++ = 0x10 | (cc & 0x0f); /* payload */
	
	return p;
}

static u_char *
mpegts_write_pes_header(u_char *p, output_frame_t* f, u_char* cur_packet_start, unsigned* pes_header_size, u_char** pes_size_ptr)
{
	unsigned header_size;
	unsigned flags;
	bool_t write_dts;

	write_dts = TRUE;

	if (f->pid == PCR_PID)
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
	*p++ = (u_char) f->sid;

	header_size = SIZEOF_PES_PTS;
	flags = 0x80; /* PTS */

	if (write_dts) 
	{
		header_size += SIZEOF_PES_PTS;
		flags |= 0x40; /* DTS */
	}

	*pes_header_size = header_size;
	*pes_size_ptr = p;
	p += 2;		// skip pes_size, updated later
	*p++ = 0x80; /* H222 */
	*p++ = (u_char) flags;
	*p++ = (u_char) header_size;

	p = mpegts_write_pts(p, flags >> 6, f->pts + INITIAL_DTS);

	if (write_dts) 
	{
		p = mpegts_write_pts(p, 1, f->dts + INITIAL_DTS);
	}
	
	return p;
}

static u_char* 
mpegts_add_stuffing(u_char* packet, u_char* p, unsigned stuff_size)
{
	u_char* packet_end = packet + MPEGTS_PACKET_SIZE;
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
	return packet_end;
}

////////////////////////////////////

// stateful functions
vod_status_t 
mpegts_encoder_init(
	mpegts_encoder_state_t* state, 
	request_context_t* request_context, 
	uint32_t segment_index, 
	write_callback_t write_callback, 
	void* write_context)
{
	u_char* cur_packet;

	vod_memzero(state, sizeof(*state));
	state->request_context = request_context;
	if (request_context->simulation_only)
	{
		return VOD_OK;
	}

	write_buffer_queue_init(&state->queue, request_context);
	state->queue.write_callback = write_callback;
	state->queue.write_context = write_context;	
	
	// append PAT packet
	cur_packet = write_buffer_queue_get_buffer(&state->queue, MPEGTS_PACKET_SIZE);
	if (cur_packet == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mpegts_encoder_init: write_buffer_queue_get_buffer failed");
		return VOD_ALLOC_FAILED;
	}

	vod_memcpy(cur_packet, pat_packet, sizeof(pat_packet));
	vod_memset(cur_packet + sizeof(pat_packet), 0xff, MPEGTS_PACKET_SIZE - sizeof(pat_packet));

	// make the continuity counters of the PAT/PMT are continous between segments
	cur_packet[3] |= (segment_index & 0x0F);

	return VOD_OK;
}

vod_status_t 
mpegts_encoder_init_streams(mpegts_encoder_state_t* state, mpegts_encoder_init_streams_state_t* stream_state, uint32_t segment_index)
{
	stream_state->request_context = state->request_context;
	stream_state->cur_pid = PCR_PID;
	stream_state->cur_video_sid = FIRST_VIDEO_SID;
	stream_state->cur_audio_sid = FIRST_AUDIO_SID;

	if (state->request_context->simulation_only)
	{
		stream_state->pmt_packet_start = NULL;
		return VOD_OK;
	}

	// append PMT packet
	stream_state->pmt_packet_start = write_buffer_queue_get_buffer(&state->queue, MPEGTS_PACKET_SIZE);
	if (stream_state->pmt_packet_start == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"mpegts_encoder_init_streams: write_buffer_queue_get_buffer failed");
		return VOD_ALLOC_FAILED;
	}
	stream_state->pmt_packet_end = stream_state->pmt_packet_start + MPEGTS_PACKET_SIZE;

	vod_memcpy(stream_state->pmt_packet_start, pmt_header_template, sizeof(pmt_header_template));
	stream_state->pmt_packet_start[3] |= (segment_index & 0x0F);
	stream_state->pmt_packet_pos = stream_state->pmt_packet_start + sizeof(pmt_header_template);

	return VOD_OK;
}

vod_status_t 
mpegts_encoder_add_stream(mpegts_encoder_init_streams_state_t* stream_state, int media_type, unsigned* pid, unsigned* sid)
{
	const u_char* pmt_entry;
	int pmt_entry_size;

	*pid = stream_state->cur_pid++;
	switch (media_type)
	{
	case MEDIA_TYPE_VIDEO:
		*sid = stream_state->cur_video_sid++;
		pmt_entry = pmt_entry_template_h264;
		pmt_entry_size = sizeof(pmt_entry_template_h264);
		break;

	case MEDIA_TYPE_AUDIO:
		*sid = stream_state->cur_audio_sid++;
		pmt_entry = pmt_entry_template_aac;
		pmt_entry_size = sizeof(pmt_entry_template_aac);
		break;

	default:
		vod_log_error(VOD_LOG_ERR, stream_state->request_context->log, 0,
			"mpegts_encoder_add_stream: invalid media type %d", media_type);
		return VOD_UNEXPECTED;
	}

	if (stream_state->pmt_packet_start == NULL)			// simulation only
	{
		return VOD_OK;
	}

	if (stream_state->pmt_packet_pos + pmt_entry_size + sizeof(pmt_entry_template_id3) + sizeof(uint32_t) >= 
		stream_state->pmt_packet_end)
	{
		vod_log_error(VOD_LOG_ERR, stream_state->request_context->log, 0,
			"mpegts_encoder_add_stream: stream definitions overflow PMT size");
		return VOD_BAD_DATA;
	}

	vod_memcpy(stream_state->pmt_packet_pos, pmt_entry, pmt_entry_size);
	pmt_entry_set_elementary_pid(stream_state->pmt_packet_pos, *pid);
	stream_state->pmt_packet_pos += pmt_entry_size;
	return VOD_OK;
}

void 
mpegts_encoder_finalize_streams(mpegts_encoder_init_streams_state_t* stream_state)
{
	u_char* p = stream_state->pmt_packet_pos;
	u_char* crc_start_offset;
	uint32_t crc;

	if (stream_state->pmt_packet_start == NULL)			// simulation only
	{
		return;
	}

	// append id3 stream
	vod_memcpy(p, pmt_entry_template_id3, sizeof(pmt_entry_template_id3));
	pmt_entry_set_elementary_pid(p, stream_state->cur_pid);
	p += sizeof(pmt_entry_template_id3);

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
	memset(p, 0xFF, stream_state->pmt_packet_end - p);
}

static vod_status_t 
mpegts_encoder_init_packet(mpegts_encoder_state_t* state, bool_t first)
{
	state->cur_packet_start = write_buffer_queue_get_buffer(&state->queue, MPEGTS_PACKET_SIZE);
	if (state->cur_packet_start == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"mpegts_encoder_init_packet: write_buffer_queue_get_buffer failed");
		return VOD_ALLOC_FAILED;
	}
	state->cur_packet_end = state->cur_packet_start + MPEGTS_PACKET_SIZE;
	state->cur_pos = mpegts_write_packet_header(state->cur_packet_start, state->pid, *state->cc, first);
	(*state->cc)++;
	return VOD_OK;
}

static vod_status_t 
mpegts_encoder_start_frame(void* context, output_frame_t* frame)
{
	mpegts_encoder_state_t* state = (mpegts_encoder_state_t*)context;
	vod_status_t rc;

	state->pid = frame->pid;
	state->cc = frame->cc;
	state->last_stream_frame = frame->last_stream_frame;
	rc = mpegts_encoder_init_packet(state, TRUE);
	if (rc != VOD_OK)
	{
		return rc;
	}

	state->cur_pos = mpegts_write_pes_header(state->cur_pos, frame, state->cur_packet_start, &state->cur_pes_header_size, &state->cur_pes_size_ptr);
	state->pes_bytes_written = 0;
	
	return VOD_OK;
}

static vod_status_t 
mpegts_encoder_write(void* context, const u_char* buffer, uint32_t size)
{
	mpegts_encoder_state_t* state = (mpegts_encoder_state_t*)context;
	uint32_t input_buffer_left;
	uint32_t packet_size_left;
	uint32_t cur_size;
	const u_char* buffer_end = buffer + size;
	vod_status_t rc;

	for (;;)
	{
		input_buffer_left = buffer_end - buffer;
		if (input_buffer_left <= 0)
		{
			break;
		}
		
		if (state->cur_pos >= state->cur_packet_end)
		{
			rc = mpegts_encoder_init_packet(state, FALSE);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		
		// copy as much as possible to the current packet
		packet_size_left = state->cur_packet_end - state->cur_pos;
		cur_size = vod_min(packet_size_left, input_buffer_left);
		
		vod_memcpy(state->cur_pos, buffer, cur_size);
		buffer += cur_size;
		state->cur_pos += cur_size;
		state->pes_bytes_written += cur_size;
	}
	
	return VOD_OK;
}

static vod_status_t 
mpegts_append_null_packet(mpegts_encoder_state_t* state)
{
	u_char* packet;
	vod_status_t rc;

	rc = mpegts_encoder_init_packet(state, FALSE);
	if (rc != VOD_OK)
	{
		return rc;
	}

	packet = state->cur_packet_start;
	packet[3] |= 0x20;
	packet[4] = (u_char) MPEGTS_PACKET_SIZE - SIZEOF_MPEGTS_HEADER - 1;
	packet[5] = 0;
	vod_memset(&packet[6], 0xff, MPEGTS_PACKET_SIZE - SIZEOF_MPEGTS_HEADER - 2);
	return VOD_OK;
}
			
static vod_status_t 
mpegts_encoder_flush_frame(void* context, int32_t margin_size)
{
	mpegts_encoder_state_t* state = (mpegts_encoder_state_t*)context;
	unsigned pes_size;
	unsigned stuff_size;
	vod_status_t rc;

	stuff_size = state->cur_packet_end - state->cur_pos;
	if (stuff_size > 0)
	{
		state->cur_pos = mpegts_add_stuffing(state->cur_packet_start, state->cur_pos, stuff_size);
		if (state->cur_pes_size_ptr >= state->cur_packet_start && state->cur_pes_size_ptr < state->cur_packet_end)
		{
			state->cur_pes_size_ptr += stuff_size;
		}
	}
	
	// update the size in the pes header
	pes_size = SIZEOF_PES_OPTIONAL_HEADER + state->cur_pes_header_size + state->pes_bytes_written;
	if (pes_size > 0xffff) 
	{
		pes_size = 0;
	}
	*state->cur_pes_size_ptr++ = (u_char) (pes_size >> 8);
	*state->cur_pes_size_ptr++ = (u_char) pes_size;

	// add null packets to leave at least margin_size bytes
	if (margin_size > (int32_t)stuff_size)
	{
		margin_size -= stuff_size;
		while (margin_size > 0)
		{
			rc = mpegts_append_null_packet(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			margin_size -= (MPEGTS_PACKET_SIZE - SIZEOF_MPEGTS_HEADER);
		}
	}
	
	// on the last frame, add null packets to set the continuity counters
	if (state->last_stream_frame)
	{
		while ((*state->cc) & 0x0F)
		{
			rc = mpegts_append_null_packet(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}
	
	// send the buffer if it's full
	return write_buffer_queue_send(&state->queue, state->cur_packet_end);
}

vod_status_t 
mpegts_encoder_flush(mpegts_encoder_state_t* state)
{
	return write_buffer_queue_flush(&state->queue);
}

void 
mpegts_encoder_simulated_start_segment(mpegts_encoder_state_t* state)
{
	state->simulated_offset = 2 * MPEGTS_PACKET_SIZE;		// PAT & PMT
}

static void 
mpegts_encoder_simulated_write(void* context, output_frame_t* frame)
{
	mpegts_encoder_state_t* state = (mpegts_encoder_state_t*)context;
	uint32_t packet_count;

	if (frame->pid == PCR_PID)
	{
		frame->original_size += SIZEOF_MPEGTS_ADAPTATION_FIELD + SIZEOF_PCR;		// adaptation + pcr
	}

	frame->original_size += SIZEOF_PES_HEADER + SIZEOF_PES_OPTIONAL_HEADER + 2 * SIZEOF_PES_PTS;	// pts & dts

	packet_count = vod_div_ceil(frame->original_size, MPEGTS_PACKET_SIZE - SIZEOF_MPEGTS_HEADER);		// 4 = mpegts header

	(*frame->cc) += packet_count;

	if (frame->last_stream_frame && ((*frame->cc) & 0x0F) != 0)
	{
		packet_count += 0x10 - ((*frame->cc) & 0x0F);		// null packets for continuity counters
		(*frame->cc) = 0;
	}

	state->simulated_offset += packet_count * MPEGTS_PACKET_SIZE;
}

uint32_t 
mpegts_encoder_simulated_get_offset(mpegts_encoder_state_t* state)
{
	return state->simulated_offset;
}

const media_filter_t mpegts_encoder = {
	mpegts_encoder_start_frame,
	mpegts_encoder_write,
	mpegts_encoder_flush_frame,
	mpegts_encoder_simulated_write,
};
