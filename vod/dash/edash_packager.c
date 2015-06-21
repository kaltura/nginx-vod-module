#include "edash_packager.h"
#include "dash_packager.h"
#include "../read_stream.h"
#include "../mp4/mp4_builder.h"
#include "../mp4/mp4_encrypt.h"
#include "../mp4/mp4_defs.h"
#include "../common.h"

// manifest constants
#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC									\
	"        <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\" value=\"cenc\"/>\n"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX								\
	"        <ContentProtection schemeIdUri=\"urn:uuid:"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_SUFFIX								\
	"\"/>\n"

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
	u_char default_kid[MP4_ENCRYPT_KID_SIZE];
} tenc_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char system_id[MP4_ENCRYPT_SYSTEM_ID_SIZE];
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

////// mpd functions

static u_char* 
edash_packager_write_content_protection(void* context, u_char* p, mpeg_stream_metadata_t* stream)
{
	mp4_encrypt_info_t* drm_info = (mp4_encrypt_info_t*)stream->file_info.drm_info;
	mp4_encrypt_system_info_t* cur_info;

	p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC) - 1);
	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PREFIX) - 1);
		p = mp4_encrypt_write_guid(p, cur_info->system_id);
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
	mp4_encrypt_info_t* drm_info;
	size_t representation_tags_size;
	vod_status_t rc;

	representation_tags_size = 0;
	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		drm_info = (mp4_encrypt_info_t*)cur_stream->file_info.drm_info;

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
	result->original_stsd_entry_size = parse_be32(result->original_stsd_entry->size);
	result->original_stsd_entry_format = parse_be32(result->original_stsd_entry->format);

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
	p = vod_copy(p, context->default_kid, MP4_ENCRYPT_KID_SIZE);			// default key id

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
	mp4_encrypt_system_info_array_t* pssh_array = (mp4_encrypt_system_info_array_t*)context;
	mp4_encrypt_system_info_t* cur_info;
	size_t pssh_atom_size;

	for (cur_info = pssh_array->first; cur_info < pssh_array->last; cur_info++)
	{
		pssh_atom_size = ATOM_HEADER_SIZE + sizeof(pssh_atom_t) + cur_info->data.len;

		write_atom_header(p, pssh_atom_size, 'p', 's', 's', 'h');
		write_dword(p, 0);						// version + flags
		p = vod_copy(p, cur_info->system_id, MP4_ENCRYPT_SYSTEM_ID_SIZE);	// system id
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
	mp4_encrypt_info_t* drm_info = (mp4_encrypt_info_t*)mpeg_metadata->first_stream->file_info.drm_info;
	atom_writer_t pssh_atom_writer;
	atom_writer_t stsd_atom_writer;
	stsd_writer_context_t stsd_writer_context;
	mp4_encrypt_system_info_t* cur_info;
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

////// video fragment functions

static u_char*
edash_packager_video_write_auxiliary_data(void* context, u_char* p)
{
	mp4_encrypt_video_state_t* state = (mp4_encrypt_video_state_t*)context;

	p = vod_copy(p, state->auxiliary_data.start, state->auxiliary_data.pos - state->auxiliary_data.start);

	return p;
}

static vod_status_t
edash_packager_video_write_fragment_header(mp4_encrypt_video_state_t* state)
{
	atom_writer_t auxiliary_data_writer;
	vod_str_t fragment_header;
	vod_status_t rc;
	size_t total_fragment_size;
	bool_t reuse_buffer;

	// get the auxiliary data writer
	auxiliary_data_writer.atom_size = state->auxiliary_data.pos - state->auxiliary_data.start;
	auxiliary_data_writer.write = edash_packager_video_write_auxiliary_data;
	auxiliary_data_writer.context = state;

	// build the fragment header
	rc = dash_packager_build_fragment_header(
		state->base.request_context,
		state->base.stream_metadata,
		state->base.segment_index,
		0,
		state->base.saiz_atom_size + state->base.saio_atom_size,
		(write_extra_traf_atoms_callback_t)mp4_encrypt_video_write_saiz_saio,
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

////// audio fragment functions

static vod_status_t
edash_packager_audio_build_fragment_header(
	mp4_encrypt_state_t* state,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size)
{
	atom_writer_t auxiliary_data_writer;
	vod_status_t rc;

	auxiliary_data_writer.atom_size = MP4_ENCRYPT_IV_SIZE * state->stream_metadata->frame_count;
	auxiliary_data_writer.write = (atom_writer_func_t)mp4_encrypt_audio_write_auxiliary_data;
	auxiliary_data_writer.context = state;

	rc = dash_packager_build_fragment_header(
		state->request_context,
		state->stream_metadata,
		state->segment_index,
		0,
		state->saiz_atom_size + state->saio_atom_size,
		(write_extra_traf_atoms_callback_t)mp4_encrypt_audio_write_saiz_saio,
		state,
		&auxiliary_data_writer,
		size_only,
		fragment_header,
		total_fragment_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"edash_packager_audio_build_fragment_header: dash_packager_build_fragment_header failed %i", rc);
		return rc;
	}

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
	vod_status_t rc;

	switch (stream_metadata->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		return mp4_encrypt_video_get_fragment_writer(
			result, 
			request_context, 
			stream_metadata, 
			segment_index, 
			edash_packager_video_write_fragment_header,
			segment_writer, 
			iv);

	case MEDIA_TYPE_AUDIO:
		rc = mp4_encrypt_audio_get_fragment_writer(
			result, 
			request_context, 
			stream_metadata, 
			segment_index, 
			segment_writer, 
			iv);
		if (rc != VOD_OK)
		{
			return rc;
		}

		rc = edash_packager_audio_build_fragment_header(
			result->context,
			size_only,
			fragment_header,
			total_fragment_size);
		if (rc != VOD_OK)
		{
			return rc;
		}

		return VOD_OK;
	}

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"edash_packager_get_fragment_writer: invalid media type %uD", stream_metadata->media_info.media_type);
	return VOD_UNEXPECTED;
}
