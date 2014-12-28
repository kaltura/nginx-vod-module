#include <openssl/aes.h>
#include "edash_packager.h"
#include "dash_packager.h"
#include "../dynamic_buffer.h"
#include "../read_stream.h"
#include "../mp4_builder.h"
#include "../common.h"

// manifest constants
#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC									\
	"        <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\" value=\"cenc\"/>\n"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX								\
	"        <ContentProtection schemeIdUri=\"urn:uuid:"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_SUFFIX								\
	"\"/>\n"

#define VOD_GUID_LENGTH (sizeof("00000000-0000-0000-0000-000000000000") - 1)

// encryption constants
#define IV_SIZE (8)
#define COUNTER_SIZE (AES_BLOCK_SIZE)

// init segment types
typedef struct {
	u_char data_format[4];
} frma_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char scheme_type[4];
	u_char scheme_version[4];
} schm_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char default_is_encrypted[3];
	u_char default_iv_size;
	u_char default_kid[EDASH_KID_SIZE];
} tenc_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char system_id[EDASH_SYSTEM_ID_SIZE];
	u_char data_size[4];
} pssh_atom_t;

typedef struct {
	uint32_t media_type;
	bool_t has_clear_lead;
	u_char* default_kid;
	stsd_entry_header_t* original_stsd_entry;
	uint32_t original_stsd_entry_size;
	uint32_t original_stsd_entry_format;
	size_t tenc_atom_size;
	size_t schi_atom_size;
	size_t schm_atom_size;
	size_t frma_atom_size;
	size_t sinf_atom_size;
	size_t encrypted_stsd_entry_size;
	size_t stsd_atom_size;
} stsd_writer_context_t;

// fragment types
typedef struct {
	u_char iv[IV_SIZE];
	u_char subsample_count[2];
} cenc_sample_auxiliary_data_t;

typedef struct {
	u_char bytes_of_clear_data[2];
	u_char bytes_of_encrypted_data[4];
} cenc_sample_auxiliary_data_subsample_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char default_sample_info_size;
	u_char sample_count[4];
} saiz_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char entry_count[4];
	u_char offset[4];
} saio_atom_t;

// fragment writer state
enum {
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_PACKET_DATA,
};

typedef struct {
	segment_writer_t segment_writer;

	// fixed
	request_context_t* request_context;

	// encryption state
	u_char iv[IV_SIZE];
	u_char counter[COUNTER_SIZE];
	u_char encrypted_counter[COUNTER_SIZE];
	int block_offset;
	AES_KEY encryption_key;

	// frame state
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t frame_count;
	uint32_t frame_size_left;
} edash_packager_state_t;

typedef struct {
	edash_packager_state_t base;

	// fixed
	mpeg_stream_metadata_t* stream_metadata;
	uint32_t segment_index;
	uint32_t nal_packet_size_length;

	// auxiliary data state
	vod_buf_t auxiliary_data;
	u_char* auxiliary_sample_sizes;
	u_char* auxiliary_sample_sizes_pos;
	uint16_t subsample_count;

	// saiz / saio atoms
	u_char default_auxiliary_sample_size;
	uint32_t saiz_sample_count;
	size_t saiz_atom_size;
	size_t saio_atom_size;

	// nal packet state
	int cur_state;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;
} edash_packager_video_state_t;

////// mpd functions

static u_char*
edash_packager_write_guid(u_char* p, u_char* guid)
{
	p = vod_sprintf(p, "%02xd%02xd%02xd%02xd-%02xd%02xd-%02xd%02xd-%02xd%02xd-%02xd%02xd%02xd%02xd%02xd%02xd",
		guid[0], guid[1], guid[2], guid[3],
		guid[4], guid[5],
		guid[6], guid[7],
		guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
	return p;
}

static u_char* 
edash_packager_write_content_protection(void* context, u_char* p, mpeg_stream_metadata_t* stream)
{
	edash_drm_info_t* drm_info = (edash_drm_info_t*)stream->file_info.drm_info;
	edash_pssh_info_t* cur_info;

	p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC) - 1);
	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX) - 1);
		p = edash_packager_write_guid(p, cur_info->system_id);
		p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_SUFFIX, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_SUFFIX) - 1);
	}
	return p;
}

vod_status_t
edash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	mpeg_stream_metadata_t* cur_stream;
	edash_drm_info_t* drm_info;
	size_t representation_tags_size;
	vod_status_t rc;

	representation_tags_size = 0;
	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		drm_info = (edash_drm_info_t*)cur_stream->file_info.drm_info;

		representation_tags_size += sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC) - 1;
		representation_tags_size +=
			(sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX) - 1 +
				VOD_GUID_LENGTH +
			sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_SUFFIX) - 1) * drm_info->pssh_array.count;
	}

	rc = dash_packager_build_mpd(
		request_context,
		conf,
		base_url,
		segmenter_conf,
		mpeg_metadata,
		representation_tags_size,
		edash_packager_write_content_protection,
		NULL,
		result);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_build_mpd: dash_packager_build_mpd failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

////// init segment functions

static vod_status_t
edash_packager_init_stsd_writer_context(
	request_context_t* request_context,
	uint32_t media_type, 
	raw_atom_t* original_stsd, 
	bool_t has_clear_lead,
	u_char* default_kid,
	stsd_writer_context_t* result)
{
	result->media_type = media_type;
	result->has_clear_lead = has_clear_lead;
	result->default_kid = default_kid;

	if (original_stsd->size < original_stsd->header_size + sizeof(stsd_atom_t) + sizeof(stsd_entry_header_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"edash_packager_init_stsd_writer_context: invalid stsd size %uL", original_stsd->size);
		return VOD_BAD_DATA;
	}

	result->original_stsd_entry = (stsd_entry_header_t*)(original_stsd->ptr + original_stsd->header_size + sizeof(stsd_atom_t));
	result->original_stsd_entry_size = PARSE_BE32(result->original_stsd_entry->size);
	result->original_stsd_entry_format = PARSE_BE32(result->original_stsd_entry->format);

	if (result->original_stsd_entry_size < sizeof(stsd_entry_header_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"edash_packager_init_stsd_writer_context: invalid stsd entry size %uD", result->original_stsd_entry_size);
		return VOD_BAD_DATA;
	}

	result->tenc_atom_size = ATOM_HEADER_SIZE + sizeof(tenc_atom_t);
	result->schi_atom_size = ATOM_HEADER_SIZE + result->tenc_atom_size;
	result->schm_atom_size = ATOM_HEADER_SIZE + sizeof(schm_atom_t);
	result->frma_atom_size = ATOM_HEADER_SIZE + sizeof(frma_atom_t);
	result->sinf_atom_size = ATOM_HEADER_SIZE + 
		result->frma_atom_size + 
		result->schm_atom_size + 
		result->schi_atom_size;
	result->encrypted_stsd_entry_size = result->original_stsd_entry_size + result->sinf_atom_size;
	result->stsd_atom_size = ATOM_HEADER_SIZE + sizeof(stsd_atom_t) + result->encrypted_stsd_entry_size;
	if (has_clear_lead)
	{
		result->stsd_atom_size += result->original_stsd_entry_size;
	}

	return VOD_OK;
}

static u_char*
edash_packager_write_stsd(void* ctx, u_char* p)
{
	stsd_writer_context_t* context = (stsd_writer_context_t*)ctx;
	u_char format_by_media_type[MEDIA_TYPE_COUNT] = { 'v', 'a' };

	// stsd
	write_atom_header(p, context->stsd_atom_size, 's', 't', 's', 'd');
	write_dword(p, 0);		// version + flags
	write_dword(p, context->has_clear_lead ? 2 : 1);		// entries

	// stsd encrypted entry
	write_dword(p, context->encrypted_stsd_entry_size);		// size
	write_atom_name(p, 'e', 'n', 'c', format_by_media_type[context->media_type]);	// format
	p = vod_copy(p, context->original_stsd_entry + 1, context->original_stsd_entry_size - sizeof(stsd_entry_header_t));

	// sinf
	write_atom_header(p, context->sinf_atom_size, 's', 'i', 'n', 'f');
	
	// sinf.frma
	write_atom_header(p, context->frma_atom_size, 'f', 'r', 'm', 'a');
	write_dword(p, context->original_stsd_entry_format);

	// sinf.schm
	write_atom_header(p, context->schm_atom_size, 's', 'c', 'h', 'm');
	write_dword(p, 0);							// version + flags
	write_atom_name(p, 'c', 'e', 'n', 'c');		// scheme type
	write_dword(p, 0x10000);					// scheme version

	// sinf.schi
	write_atom_header(p, context->schi_atom_size, 's', 'c', 'h', 'i');

	// sinf.schi.tenc
	write_atom_header(p, context->tenc_atom_size, 't', 'e', 'n', 'c');
	write_dword(p, 0);							// version + flags
	write_dword(p, 0x108);						// default is encrypted (1) + iv size (8)
	p = vod_copy(p, context->default_kid, EDASH_KID_SIZE);			// default key id

	// clear entry
	if (context->has_clear_lead)
	{
		p = vod_copy(p, context->original_stsd_entry, context->original_stsd_entry_size);
	}

	return p;
}

static u_char*
edash_packager_write_pssh(void* context, u_char* p)
{
	edash_pssh_info_array_t* pssh_array = (edash_pssh_info_array_t*)context;
	edash_pssh_info_t* cur_info;
	size_t pssh_atom_size;

	for (cur_info = pssh_array->first; cur_info < pssh_array->last; cur_info++)
	{
		pssh_atom_size = ATOM_HEADER_SIZE + sizeof(pssh_atom_t) + cur_info->data.len;

		write_atom_header(p, pssh_atom_size, 'p', 's', 's', 'h');
		write_dword(p, 0);						// version + flags
		p = vod_copy(p, cur_info->system_id, EDASH_SYSTEM_ID_SIZE);	// system id
		write_dword(p, cur_info->data.len);		// data size
		p = vod_copy(p, cur_info->data.data, cur_info->data.len);
	}

	return p;
}

vod_status_t
edash_packager_build_init_mp4(
	request_context_t* request_context,
	mpeg_metadata_t* mpeg_metadata,
	bool_t has_clear_lead,
	bool_t size_only,
	vod_str_t* result)
{
	edash_drm_info_t* drm_info = (edash_drm_info_t*)mpeg_metadata->first_stream->file_info.drm_info;
	atom_writer_t pssh_atom_writer;
	atom_writer_t stsd_atom_writer;
	stsd_writer_context_t stsd_writer_context;
	edash_pssh_info_t* cur_info;
	vod_status_t rc;

	rc = edash_packager_init_stsd_writer_context(
		request_context,
		mpeg_metadata->first_stream->media_info.media_type,
		&mpeg_metadata->first_stream->raw_atoms[RTA_STSD], 
		has_clear_lead,
		drm_info->key_id,
		&stsd_writer_context);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_build_init_mp4: edash_packager_init_stsd_writer_context failed %i", rc);
		return rc;
	}

	// build the pssh writer
	pssh_atom_writer.atom_size = 0;
	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		pssh_atom_writer.atom_size += ATOM_HEADER_SIZE + sizeof(pssh_atom_t) + cur_info->data.len;
	}
	pssh_atom_writer.write = edash_packager_write_pssh;
	pssh_atom_writer.context = &drm_info->pssh_array;

	// build the stsd writer
	stsd_atom_writer.atom_size = stsd_writer_context.stsd_atom_size;
	stsd_atom_writer.write = edash_packager_write_stsd;
	stsd_atom_writer.context = &stsd_writer_context;

	rc = dash_packager_build_init_mp4(
		request_context,
		mpeg_metadata,
		size_only,
		&pssh_atom_writer,
		&stsd_atom_writer,
		result);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_build_init_mp4: dash_packager_build_init_mp4 failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

////// fragment common functions

static void
increment_be64(u_char* counter)
{
	u_char* cur_pos;

	for (cur_pos = counter + 7; cur_pos >= counter; cur_pos--)
	{
		(*cur_pos)++;
		if (*cur_pos != 0)
		{
			break;
		}
	}
}

static void
edash_packager_encrypt(edash_packager_state_t* state, u_char* buffer, uint32_t size)
{
	u_char* encrypted_counter_pos;
	u_char* cur_end_pos;
	u_char* buffer_end = buffer + size;

	while (buffer < buffer_end)
	{
		if (state->block_offset == 0)
		{
			AES_encrypt(state->counter, state->encrypted_counter, &state->encryption_key);
			increment_be64(state->counter + 8);
		}

		encrypted_counter_pos = state->encrypted_counter + state->block_offset;
		cur_end_pos = buffer + COUNTER_SIZE - state->block_offset;
		cur_end_pos = MIN(cur_end_pos, buffer_end);

		state->block_offset += cur_end_pos - buffer;
		state->block_offset &= (COUNTER_SIZE - 1);

		while (buffer < cur_end_pos)
		{
			*buffer ^= *encrypted_counter_pos;
			buffer++;
			encrypted_counter_pos++;
		}
	}
}

static vod_status_t
edash_packager_init_state(
	edash_packager_state_t* state,
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	segment_writer_t* segment_writer,
	const u_char* iv)
{
	edash_drm_info_t* drm_info = (edash_drm_info_t*)stream_metadata->file_info.drm_info;
	uint64_t iv_int;
	u_char* p;

	// fixed fields
	state->request_context = request_context;
	state->segment_writer = *segment_writer;
	if (AES_set_encrypt_key(drm_info->key, EDASH_AES_KEY_SIZE * 8, &state->encryption_key) != 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"edash_packager_init_state: AES_set_encrypt_key failed");
		return VOD_UNEXPECTED;
	}

	// increment the iv by the index of the first frame
	iv_int = PARSE_BE64(iv);
	iv_int += stream_metadata->first_frame_index;
	p = state->iv;
	write_qword(p, iv_int);
	
	// frame state
	state->cur_frame = stream_metadata->frames;
	state->last_frame = stream_metadata->frames + stream_metadata->frame_count;
	state->frame_count = stream_metadata->frame_count;
	state->frame_size_left = 0;

	return VOD_OK;
}

static vod_status_t
edash_packager_start_frame(edash_packager_state_t* state)
{
	// get the frame size
	if (state->cur_frame >= state->last_frame)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"edash_packager_start_frame: no more frames");
		return VOD_BAD_DATA;
	}

	state->frame_size_left = state->cur_frame->size;
	state->cur_frame++;

	// initialize the counter and block offset
	vod_memcpy(state->counter, state->iv, sizeof(state->iv));
	vod_memzero(state->counter + sizeof(state->iv), sizeof(state->counter) - sizeof(state->iv));
	state->block_offset = 0;

	// increment the iv
	increment_be64(state->iv);

	return VOD_OK;
}

////// video fragment functions

static vod_status_t
edash_packager_video_start_frame(edash_packager_video_state_t* state)
{
	vod_status_t rc;

	// add an auxiliary data entry
	rc = vod_buf_reserve(&state->auxiliary_data, sizeof(cenc_sample_auxiliary_data_t));
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"edash_packager_video_start_frame: vod_buf_reserve failed %i", rc);
		return rc;
	}

	state->auxiliary_data.pos = vod_copy(state->auxiliary_data.pos, state->base.iv, sizeof(state->base.iv));
	state->auxiliary_data.pos += sizeof(uint16_t);		// write the subsample count on frame end
	state->subsample_count = 0;

	// call the base start frame
	rc = edash_packager_start_frame(&state->base);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"edash_packager_video_start_frame: edash_packager_start_frame failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
edash_packager_video_add_subsample(edash_packager_video_state_t* state, uint16_t bytes_of_clear_data, uint32_t bytes_of_encrypted_data)
{
	vod_status_t rc;

	rc = vod_buf_reserve(&state->auxiliary_data, sizeof(cenc_sample_auxiliary_data_subsample_t));
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"edash_packager_video_add_subsample: vod_buf_reserve failed %i", rc);
		return rc;
	}
	write_word(state->auxiliary_data.pos, bytes_of_clear_data);
	write_dword(state->auxiliary_data.pos, bytes_of_encrypted_data);
	state->subsample_count++;

	return VOD_OK;
}

static vod_status_t
edash_packager_video_end_frame(edash_packager_video_state_t* state)
{
	size_t sample_size;
	u_char* p;

	// add the sample size to saiz
	sample_size = sizeof(cenc_sample_auxiliary_data_t) +
		state->subsample_count * sizeof(cenc_sample_auxiliary_data_subsample_t);
	*(state->auxiliary_sample_sizes_pos)++ = sample_size;

	// update subsample count in auxiliary_data
	p = state->auxiliary_data.pos - sample_size + offsetof(cenc_sample_auxiliary_data_t, subsample_count);
	write_word(p, state->subsample_count);

	return VOD_OK;
}

static u_char* 
edash_packager_video_write_extra_traf_atoms(void* context, u_char* p, size_t moof_atom_size)
{
	edash_packager_video_state_t* state = (edash_packager_video_state_t*)context;

	// moof.traf.saiz
	write_atom_header(p, state->saiz_atom_size, 's', 'a', 'i', 'z');
	write_dword(p, 0);			// version, flags
	*p++ = state->default_auxiliary_sample_size;
	write_dword(p, state->saiz_sample_count);
	if (state->default_auxiliary_sample_size == 0)
	{
		p = vod_copy(p, state->auxiliary_sample_sizes, state->saiz_sample_count);
	}

	// moof.traf.saio
	write_atom_header(p, state->saio_atom_size, 's', 'a', 'i', 'o');
	write_dword(p, 0);			// version, flags
	write_dword(p, 1);			// entry count
	write_dword(p, moof_atom_size + ATOM_HEADER_SIZE);		// offset (moof start to auxiliary data start)

	return p;
}

static void 
edash_packager_video_calc_default_auxiliary_sample_size(edash_packager_video_state_t* state)
{
	u_char* cur_pos;

	if (state->auxiliary_sample_sizes >= state->auxiliary_sample_sizes_pos)
	{
		state->default_auxiliary_sample_size = 0;
		return;
	}

	state->default_auxiliary_sample_size = *state->auxiliary_sample_sizes;
	for (cur_pos = state->auxiliary_sample_sizes + 1; cur_pos < state->auxiliary_sample_sizes_pos; cur_pos++)
	{
		if (*cur_pos != state->default_auxiliary_sample_size)
		{
			state->default_auxiliary_sample_size = 0;
			break;
		}
	}
}

static u_char*
edash_packager_video_write_auxiliary_data(void* context, u_char* p)
{
	edash_packager_video_state_t* state = (edash_packager_video_state_t*)context;

	p = vod_copy(p, state->auxiliary_data.start, state->auxiliary_data.pos - state->auxiliary_data.start);

	return p;
}

static vod_status_t
edash_packager_video_write_fragment_header(edash_packager_video_state_t* state)
{
	atom_writer_t auxiliary_data_writer;
	vod_str_t fragment_header;
	vod_status_t rc;
	size_t total_fragment_size;
	bool_t reuse_buffer;

	// calculate atom sizes
	edash_packager_video_calc_default_auxiliary_sample_size(state);
	state->saiz_sample_count = state->auxiliary_sample_sizes_pos - state->auxiliary_sample_sizes;
	state->saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	if (state->default_auxiliary_sample_size == 0)
	{
		state->saiz_atom_size += state->saiz_sample_count;
	}
	state->saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);

	// get the auxiliary data writer
	auxiliary_data_writer.atom_size = state->auxiliary_data.pos - state->auxiliary_data.start;
	auxiliary_data_writer.write = edash_packager_video_write_auxiliary_data;
	auxiliary_data_writer.context = state;

	// build the fragment header
	rc = dash_packager_build_fragment_header(
		state->base.request_context,
		state->stream_metadata,
		state->segment_index,
		0,
		state->saiz_atom_size + state->saio_atom_size,
		edash_packager_video_write_extra_traf_atoms,
		state,
		&auxiliary_data_writer,
		FALSE,
		&fragment_header,
		&total_fragment_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"edash_packager_video_write_fragment_header: dash_packager_build_fragment_header failed %i", rc);
		return rc;
	}

	rc = state->base.segment_writer.write_head(
		state->base.segment_writer.context,
		fragment_header.data, 
		fragment_header.len, 
		&reuse_buffer);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"edash_packager_video_write_fragment_header: write_head failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

static vod_status_t 
edash_packager_video_write_buffer(void* context, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	edash_packager_video_state_t* state = (edash_packager_video_state_t*)context;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	uint32_t write_size;
	vod_status_t rc;
	
	while (cur_pos < buffer_end)
	{
		switch (state->cur_state)
		{
		case STATE_PACKET_SIZE:
			if (state->base.frame_size_left <= 0)
			{
				for (;;)
				{
					rc = edash_packager_video_start_frame(state);
					if (rc != VOD_OK)
					{
						return rc;
					}

					if (state->base.frame_size_left > 0)
					{
						break;
					}
					
					rc = edash_packager_video_end_frame(state);
					if (rc != VOD_OK)
					{
						return rc;
					}
				}
			}

			for (; state->length_bytes_left && cur_pos < buffer_end; state->length_bytes_left--)
			{
				state->packet_size_left = (state->packet_size_left << 8) | *cur_pos++;
			}

			if (cur_pos >= buffer_end)
			{
				break;
			}

			if (state->base.frame_size_left < state->nal_packet_size_length + state->packet_size_left)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"edash_packager_video_write_buffer: frame size %uD too small, nalu size %uD packet size %uD", 
					state->base.frame_size_left, state->nal_packet_size_length, state->packet_size_left);
				return VOD_BAD_DATA;
			}

			state->base.frame_size_left -= state->nal_packet_size_length + state->packet_size_left;

			state->cur_state++;
			// fall through

		case STATE_NAL_TYPE:
			cur_pos++;
			if (state->packet_size_left <= 0)
			{
				vod_log_error(VOD_LOG_ERR, state->base.request_context->log, 0,
					"edash_packager_video_write_buffer: zero size packet");
				return VOD_BAD_DATA;
			}
			state->packet_size_left--;
			
			rc = edash_packager_video_add_subsample(state, state->nal_packet_size_length + 1, state->packet_size_left);
			if (rc != VOD_OK)
			{
				return rc;
			}
			state->cur_state++;
			// fall through

		case STATE_PACKET_DATA:
			write_size = MIN(state->packet_size_left, buffer_end - cur_pos);
			
			edash_packager_encrypt(&state->base, cur_pos, write_size);
			
			cur_pos += write_size;
			state->packet_size_left -= write_size;
			if (state->packet_size_left > 0)
			{
				break;
			}

			// finished a packet
			state->cur_state = STATE_PACKET_SIZE;
			state->length_bytes_left = state->nal_packet_size_length;
			state->packet_size_left = 0;

			if (state->base.frame_size_left > 0)
			{
				break;
			}

			// finished a frame
			rc = edash_packager_video_end_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (state->base.cur_frame < state->base.last_frame)
			{
				break;
			}

			// finished all frames
			rc = edash_packager_video_write_fragment_header(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
			break;
		}
	}
			
	return state->base.segment_writer.write_tail(state->base.segment_writer.context, buffer, size, reuse_buffer);
}

static vod_status_t
edash_packager_video_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv)
{
	edash_packager_video_state_t* state;
	vod_status_t rc;
	uint32_t initial_size;

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_video_get_fragment_writer: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	rc = edash_packager_init_state(&state->base, request_context, stream_metadata, segment_writer, iv);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_video_get_fragment_writer: edash_packager_init_state failed %i", rc);
		return rc;
	}

	// fixed members
	state->stream_metadata = stream_metadata;
	state->segment_index = segment_index;
	state->nal_packet_size_length = stream_metadata->media_info.u.video.nal_packet_size_length;

	if (state->nal_packet_size_length < 1 || state->nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"edash_packager_video_get_fragment_writer: invalid nal packet size length %uD", state->nal_packet_size_length);
		return VOD_BAD_DATA;
	}

	// for progressive AVC a frame usually contains a single nalu, except the first frame which may contain codec copyright info
	initial_size = 
		(sizeof(cenc_sample_auxiliary_data_t) + sizeof(cenc_sample_auxiliary_data_subsample_t)) * stream_metadata->frame_count + 
		sizeof(cenc_sample_auxiliary_data_subsample_t);
	rc = vod_buf_init(&state->auxiliary_data, request_context, initial_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_video_get_fragment_writer: vod_buf_init failed %i", rc);
		return rc;
	}

	state->auxiliary_sample_sizes = vod_alloc(request_context->pool, stream_metadata->frame_count);
	if (state->auxiliary_sample_sizes == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_video_get_fragment_writer: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}
	state->auxiliary_sample_sizes_pos = state->auxiliary_sample_sizes;

	state->cur_state = STATE_PACKET_SIZE;
	state->length_bytes_left = state->nal_packet_size_length;
	state->packet_size_left = 0;

	if (state->base.cur_frame >= state->base.last_frame)
	{
		// an empty segment - write won't be called so we need to write the header here
		rc = edash_packager_video_write_fragment_header(state);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"edash_packager_video_get_fragment_writer: edash_packager_video_write_fragment_header failed %i", rc);
			return rc;
		}
	}

	result->write_tail = edash_packager_video_write_buffer;
	result->write_head = NULL;
	result->context = state;

	return VOD_OK;
}

////// audio fragment functions

static vod_status_t
edash_packager_audio_write_buffer(void* context, u_char* buffer, uint32_t size, bool_t* reuse_buffer)
{
	edash_packager_state_t* state = (edash_packager_state_t*)context;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	uint32_t write_size;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		while (state->frame_size_left <= 0)
		{
			rc = edash_packager_start_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		write_size = MIN(state->frame_size_left, buffer_end - cur_pos);

		edash_packager_encrypt(state, cur_pos, write_size);

		cur_pos += write_size;
		state->frame_size_left -= write_size;
	}

	return state->segment_writer.write_tail(state->segment_writer.context, buffer, size, reuse_buffer);
}

static u_char*
edash_packager_audio_write_extra_traf_atoms(void* context, u_char* p, size_t moof_atom_size)
{
	size_t saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	size_t saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);
	edash_packager_state_t* state = (edash_packager_state_t*)context;

	// moof.traf.saiz
	write_atom_header(p, saiz_atom_size, 's', 'a', 'i', 'z');
	write_dword(p, 0);			// version, flags
	*p++ = IV_SIZE;				// default auxiliary sample size
	write_dword(p, state->frame_count);

	// moof.traf.saio
	write_atom_header(p, saio_atom_size, 's', 'a', 'i', 'o');
	write_dword(p, 0);			// version, flags
	write_dword(p, 1);			// entry count
	write_dword(p, moof_atom_size + ATOM_HEADER_SIZE);		// offset (moof start to auxiliary data start)

	return p;
}

static u_char*
edash_packager_audio_write_auxiliary_data(void* context, u_char* p)
{
	edash_packager_state_t* state = (edash_packager_state_t*)context;
	u_char* end_pos = p + sizeof(state->iv) * state->frame_count;
	u_char iv[IV_SIZE];

	vod_memcpy(iv, state->iv, sizeof(iv));

	while (p < end_pos)
	{
		p = vod_copy(p, iv, sizeof(iv));
		increment_be64(iv);
	}

	return p;
}

static vod_status_t
edash_packager_audio_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size)
{
	edash_packager_state_t* state;
	atom_writer_t auxiliary_data_writer;
	vod_status_t rc;

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_audio_get_fragment_writer: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	rc = edash_packager_init_state(state, request_context, stream_metadata, segment_writer, iv);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_audio_get_fragment_writer: edash_packager_init_state failed %i", rc);
		return rc;
	}

	auxiliary_data_writer.atom_size = IV_SIZE * state->frame_count;
	auxiliary_data_writer.write = edash_packager_audio_write_auxiliary_data;
	auxiliary_data_writer.context = state;

	rc = dash_packager_build_fragment_header(
		request_context,
		stream_metadata,
		segment_index,
		0,
		2 * ATOM_HEADER_SIZE + sizeof(saiz_atom_t) + sizeof(saio_atom_t),
		edash_packager_audio_write_extra_traf_atoms,
		state,
		&auxiliary_data_writer,
		size_only,
		fragment_header,
		total_fragment_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_audio_get_fragment_writer: dash_packager_build_fragment_header failed %i", rc);
		return rc;
	}

	result->write_tail = edash_packager_audio_write_buffer;
	result->write_head = NULL;
	result->context = state;

	return VOD_OK;
}

////// fragment common functions

vod_status_t
edash_packager_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	segment_writer_t* segment_writer,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size)
{
	switch (stream_metadata->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		return edash_packager_video_get_fragment_writer(
			result, 
			request_context, 
			stream_metadata, 
			segment_index, 
			segment_writer, 
			iv);

	case MEDIA_TYPE_AUDIO:
		return edash_packager_audio_get_fragment_writer(
			result, 
			request_context, 
			stream_metadata, 
			segment_index, 
			segment_writer, 
			iv, 
			size_only, 
			fragment_header, 
			total_fragment_size);
	}

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"edash_packager_get_fragment_writer: invalid media type %uD", stream_metadata->media_info.media_type);
	return VOD_UNEXPECTED;
}
