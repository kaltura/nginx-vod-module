#include "edash_packager.h"
#include "dash_packager.h"
#include "../read_stream.h"
#include "../mp4/mp4_encrypt_passthrough.h"
#include "../mp4/mp4_builder.h"
#include "../mp4/mp4_encrypt.h"
#include "../mp4/mp4_defs.h"
#include "../udrm.h"
#include "../common.h"

// macros
#define edash_pssh_v1(info) \
	(vod_memcmp((info)->system_id, edash_clear_key_system_id, sizeof(edash_clear_key_system_id)) == 0)

// manifest constants
#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC									\
	"        <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\" value=\"cenc\"/>\n"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART1							\
	"        <ContentProtection xmlns:cenc=\"urn:mpeg:cenc:2013\" schemeIdUri=\"urn:uuid:"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART2							\
	"\" cenc:default_KID=\""

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART3							\
	"\">\n"																			\
	"          <cenc:pssh>"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART4							\
	"</cenc:pssh>\n"																\
	"        </ContentProtection>\n"

// TODO: remove this - always generate a default_KID for PlayReady
#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART1						\
	"        <ContentProtection xmlns:mspr=\"urn:microsoft:playready\" schemeIdUri=\"urn:uuid:"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART1					\
	"        <ContentProtection xmlns:cenc=\"urn:mpeg:cenc:2013\" xmlns:mspr=\"urn:microsoft:playready\" schemeIdUri=\"urn:uuid:"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART2					\
	"\" value=\"2.0\" cenc:default_KID=\""

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART3						\
	"\">\n"																			\
	"          <mspr:pro>"

#define VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART4						\
	"</mspr:pro>\n"																	\
	"        </ContentProtection>\n"

// mpd types
typedef struct {
	u_char* temp_buffer;
	bool_t write_playready_kid;
} write_content_protection_context_t;

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
	u_char default_kid[DRM_KID_SIZE];
} tenc_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char system_id[DRM_SYSTEM_ID_SIZE];
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

static u_char edash_playready_system_id[] = {
	0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86,
	0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95
};

static u_char edash_clear_key_system_id[] = {
	0x10, 0x77, 0xef, 0xec, 0xc0, 0xb2, 0x4d, 0x02, 
	0xac, 0xe3, 0x3c, 0x1e, 0x52, 0xe2, 0xfb, 0x4b
};

static u_char*
edash_packager_write_pssh(u_char* p, drm_system_info_t* cur_info)
{
	bool_t pssh_v1 = edash_pssh_v1(cur_info);
	size_t pssh_atom_size;

	pssh_atom_size = ATOM_HEADER_SIZE + sizeof(pssh_atom_t) + cur_info->data.len;
	if (pssh_v1)
	{
		pssh_atom_size -= sizeof(uint32_t);
	}

	write_atom_header(p, pssh_atom_size, 'p', 's', 's', 'h');
	if (pssh_v1)
	{
		write_be32(p, 0x01000000);						// version + flags
	}
	else
	{
		write_be32(p, 0);						// version + flags
	}
	p = vod_copy(p, cur_info->system_id, DRM_SYSTEM_ID_SIZE);	// system id
	if (!pssh_v1)
	{
		write_be32(p, cur_info->data.len);		// data size
	}
	p = vod_copy(p, cur_info->data.data, cur_info->data.len);
	return p;
}

static u_char* 
edash_packager_write_content_protection(void* ctx, u_char* p, media_track_t* track)
{
	write_content_protection_context_t* context = ctx;
	drm_info_t* drm_info = (drm_info_t*)track->file_info.drm_info;
	drm_system_info_t* cur_info;
	vod_str_t base64;
	vod_str_t pssh;

	p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC) - 1);
	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		if (vod_memcmp(cur_info->system_id, edash_playready_system_id, sizeof(edash_playready_system_id)) == 0)
		{
			if (context->write_playready_kid)
			{
				p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART1, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART1) - 1);
				p = mp4_encrypt_write_guid(p, cur_info->system_id);
				p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART2, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART2) - 1);
				p = mp4_encrypt_write_guid(p, drm_info->key_id);
			}
			else
			{
				p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART1, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART1) - 1);
				p = mp4_encrypt_write_guid(p, cur_info->system_id);
			}
			p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART3, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART3) - 1);

			base64.data = p;
			vod_encode_base64(&base64, &cur_info->data);
			p += base64.len;

			p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART4, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART4) - 1);
		}
		else
		{
			p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART1, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART1) - 1);
			p = mp4_encrypt_write_guid(p, cur_info->system_id);
			p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART2, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART2) - 1);
			p = mp4_encrypt_write_guid(p, drm_info->key_id);
			p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART3, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART3) - 1);

			pssh.data = context->temp_buffer;
			pssh.len = edash_packager_write_pssh(pssh.data, cur_info) - pssh.data;

			base64.data = p;
			vod_encode_base64(&base64, &pssh);
			p += base64.len;

			p = vod_copy(p, VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART4, sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART4) - 1);
		}
	}
	return p;
}

vod_status_t
edash_packager_build_mpd(
	request_context_t* request_context,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	bool_t drm_single_key,
	vod_str_t* result)
{
	write_content_protection_context_t context;
	dash_manifest_extensions_t extensions;
	media_sequence_t* cur_sequence;
	drm_system_info_t* cur_info;
	tags_writer_t content_prot_writer;
	drm_info_t* drm_info;
	size_t representation_tags_size;
	size_t cur_drm_tags_size;
	size_t cur_pssh_size;
	size_t max_pssh_size = 0;
	vod_status_t rc;

	representation_tags_size = 0;

	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		drm_info = (drm_info_t*)cur_sequence->drm_info;

		cur_drm_tags_size = sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC) - 1;

		for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
		{
			if (vod_memcmp(cur_info->system_id, edash_playready_system_id, sizeof(edash_playready_system_id)) == 0)
			{
				cur_drm_tags_size +=
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART1) - 1 +
					VOD_GUID_LENGTH +
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_V2_PART2) - 1 +
					VOD_GUID_LENGTH +
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART3) - 1 +
					vod_base64_encoded_length(cur_info->data.len) +
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_PLAYREADY_PART4) - 1;
			}
			else
			{
				cur_pssh_size = ATOM_HEADER_SIZE + sizeof(pssh_atom_t) + cur_info->data.len;
				if (cur_pssh_size > max_pssh_size)
				{
					max_pssh_size = cur_pssh_size;
				}

				cur_drm_tags_size +=
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART1) - 1 +
					VOD_GUID_LENGTH +
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART2) - 1 +
					VOD_GUID_LENGTH +
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART3) - 1 +
					vod_base64_encoded_length(cur_pssh_size) +
					sizeof(VOD_EDASH_MANIFEST_CONTENT_PROTECTION_CENC_PART4) - 1;

				continue;
			}
		}

		representation_tags_size += cur_drm_tags_size * cur_sequence->total_track_count;
	}

	context.write_playready_kid = conf->write_playready_kid;
	if (max_pssh_size > 0)
	{
		context.temp_buffer = vod_alloc(request_context->pool, max_pssh_size);
		if (context.temp_buffer == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"edash_packager_build_mpd: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
	}

	content_prot_writer.size = representation_tags_size;
	content_prot_writer.write = edash_packager_write_content_protection;
	content_prot_writer.context = &context;

	if (drm_single_key)
	{
		// write the ContentProtection tags under AdaptationSet
		extensions.adaptation_set = content_prot_writer;
		vod_memzero(&extensions.representation, sizeof(extensions.representation));
	}
	else
	{
		// write the ContentProtection tags under Representation
		vod_memzero(&extensions.adaptation_set, sizeof(extensions.adaptation_set));
		extensions.representation = content_prot_writer;
	}

	rc = dash_packager_build_mpd(
		request_context,
		conf,
		base_url,
		media_set,
		&extensions,
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
	write_be32(p, 0);								// version + flags
	write_be32(p, context->has_clear_lead ? 2 : 1);	// entries

	// stsd encrypted entry
	write_be32(p, context->encrypted_stsd_entry_size);		// size
	write_atom_name(p, 'e', 'n', 'c', format_by_media_type[context->media_type]);	// format
	p = vod_copy(p, context->original_stsd_entry + 1, context->original_stsd_entry_size - sizeof(stsd_entry_header_t));

	// sinf
	write_atom_header(p, context->sinf_atom_size, 's', 'i', 'n', 'f');
	
	// sinf.frma
	write_atom_header(p, context->frma_atom_size, 'f', 'r', 'm', 'a');
	write_be32(p, context->original_stsd_entry_format);

	// sinf.schm
	write_atom_header(p, context->schm_atom_size, 's', 'c', 'h', 'm');
	write_be32(p, 0);							// version + flags
	write_atom_name(p, 'c', 'e', 'n', 'c');		// scheme type
	write_be32(p, 0x10000);						// scheme version

	// sinf.schi
	write_atom_header(p, context->schi_atom_size, 's', 'c', 'h', 'i');

	// sinf.schi.tenc
	write_atom_header(p, context->tenc_atom_size, 't', 'e', 'n', 'c');
	write_be32(p, 0);							// version + flags
	write_be32(p, 0x108);						// default is encrypted (1) + iv size (8)
	p = vod_copy(p, context->default_kid, DRM_KID_SIZE);			// default key id

	// clear entry
	if (context->has_clear_lead)
	{
		p = vod_copy(p, context->original_stsd_entry, context->original_stsd_entry_size);
	}

	return p;
}

static u_char*
edash_packager_write_psshs(void* context, u_char* p)
{
	drm_system_info_array_t* pssh_array = (drm_system_info_array_t*)context;
	drm_system_info_t* cur_info;

	for (cur_info = pssh_array->first; cur_info < pssh_array->last; cur_info++)
	{
		p = edash_packager_write_pssh(p, cur_info);
	}

	return p;
}

vod_status_t
edash_packager_build_init_mp4(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t flags,
	bool_t size_only,
	vod_str_t* result)
{
	media_track_t* first_track = media_set->sequences[0].filtered_clips[0].first_track;
	drm_info_t* drm_info = (drm_info_t*)media_set->sequences[0].drm_info;
	stsd_writer_context_t stsd_writer_context;
	atom_writer_t pssh_atom_writer;
	atom_writer_t stsd_atom_writer;
	drm_system_info_t* cur_info;
	vod_status_t rc;

	// create an stsd atom if needed
	if (first_track->raw_atoms[RTA_STSD].size == 0)
	{
		rc = dash_packager_build_stsd_atom(request_context, first_track);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	rc = edash_packager_init_stsd_writer_context(
		request_context,
		first_track->media_info.media_type,
		&first_track->raw_atoms[RTA_STSD],
		(flags & EDASH_INIT_MP4_HAS_CLEAR_LEAD) != 0,
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
		if (edash_pssh_v1(cur_info))
		{
			pssh_atom_writer.atom_size -= sizeof(uint32_t);
		}
	}
	pssh_atom_writer.write = edash_packager_write_psshs;
	pssh_atom_writer.context = &drm_info->pssh_array;

	// build the stsd writer
	stsd_atom_writer.atom_size = stsd_writer_context.stsd_atom_size;
	stsd_atom_writer.write = edash_packager_write_stsd;
	stsd_atom_writer.context = &stsd_writer_context;

	rc = dash_packager_build_init_mp4(
		request_context,
		media_set,
		size_only,
		(flags & EDASH_INIT_MP4_WRITE_PSSH) != 0 ? &pssh_atom_writer : NULL,
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
edash_packager_video_write_encryption_atoms(void* context, u_char* p, size_t mdat_atom_start)
{
	mp4_encrypt_video_state_t* state = (mp4_encrypt_video_state_t*)context;
	size_t senc_data_size = state->auxiliary_data.pos - state->auxiliary_data.start;
	size_t senc_atom_size = ATOM_HEADER_SIZE + sizeof(senc_atom_t) + senc_data_size;

	// saiz / saio
	p = mp4_encrypt_video_write_saiz_saio(state, p, mdat_atom_start - senc_data_size);

	// senc
	write_atom_header(p, senc_atom_size, 's', 'e', 'n', 'c');
	write_be32(p, 0x2);		// flags
	write_be32(p, state->base.sequence->total_frame_count);
	p = vod_copy(p, state->auxiliary_data.start, senc_data_size);

	return p;
}

static vod_status_t
edash_packager_video_build_fragment_header(
	mp4_encrypt_video_state_t* state,
	vod_str_t* fragment_header, 
	size_t* total_fragment_size)
{
	dash_fragment_header_extensions_t header_extensions;

	// get the header extensions
	header_extensions.extra_traf_atoms_size = 
		state->base.saiz_atom_size + 
		state->base.saio_atom_size + 
		ATOM_HEADER_SIZE + sizeof(senc_atom_t) + state->auxiliary_data.pos - state->auxiliary_data.start;
	header_extensions.write_extra_traf_atoms_callback = edash_packager_video_write_encryption_atoms;
	header_extensions.write_extra_traf_atoms_context = state;

	// build the fragment header
	return dash_packager_build_fragment_header(
		state->base.request_context,
		state->base.media_set,
		state->base.segment_index,
		0,
		&header_extensions,
		FALSE,
		fragment_header,
		total_fragment_size);
}

////// audio fragment functions

static u_char*
edash_packager_audio_write_encryption_atoms(void* context, u_char* p, size_t mdat_atom_start)
{
	mp4_encrypt_state_t* state = (mp4_encrypt_state_t*)context;
	size_t senc_data_size = MP4_AES_CTR_IV_SIZE * state->sequence->total_frame_count;
	size_t senc_atom_size = ATOM_HEADER_SIZE + sizeof(senc_atom_t) + senc_data_size;

	// saiz / saio
	p = mp4_encrypt_audio_write_saiz_saio(state, p, mdat_atom_start - senc_data_size);

	// senc
	write_atom_header(p, senc_atom_size, 's', 'e', 'n', 'c');
	write_be32(p, 0x0);		// flags
	write_be32(p, state->sequence->total_frame_count);
	p = mp4_encrypt_audio_write_auxiliary_data(state, p);

	return p;
}

static vod_status_t
edash_packager_audio_build_fragment_header(
	mp4_encrypt_state_t* state,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size)
{
	dash_fragment_header_extensions_t header_extensions;
	vod_status_t rc;

	// get the header extensions
	header_extensions.extra_traf_atoms_size =
		state->saiz_atom_size + 
		state->saio_atom_size + 
		ATOM_HEADER_SIZE + sizeof(senc_atom_t) + MP4_AES_CTR_IV_SIZE * state->sequence->total_frame_count;
	header_extensions.write_extra_traf_atoms_callback = edash_packager_audio_write_encryption_atoms;
	header_extensions.write_extra_traf_atoms_context = state;

	// build the fragment header
	rc = dash_packager_build_fragment_header(
		state->request_context,
		state->media_set,
		state->segment_index,
		0,
		&header_extensions,
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

static u_char*
edash_packager_passthrough_write_encryption_atoms(void* ctx, u_char* p, size_t mdat_atom_start)
{
	mp4_encrypt_passthrough_context_t* context = ctx;
	media_clip_filtered_t* cur_clip;
	media_sequence_t* sequence = context->sequence;
	media_track_t* cur_track;
	size_t senc_atom_size;
	uint32_t flags;

	// saiz / saio
	p = mp4_encrypt_passthrough_write_saiz_saio(ctx, p, mdat_atom_start - context->auxiliary_info_size);

	// senc
	senc_atom_size = ATOM_HEADER_SIZE + sizeof(senc_atom_t) + context->auxiliary_info_size;
	write_atom_header(p, senc_atom_size, 's', 'e', 'n', 'c');
	flags = context->use_subsamples ? 0x2 : 0x0;
	write_be32(p, flags);		// flags
	write_be32(p, sequence->total_frame_count);
	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		cur_track = cur_clip->first_track;
		p = vod_copy(p, 
			cur_track->encryption_info.auxiliary_info, 
			cur_track->encryption_info.auxiliary_info_end - cur_track->encryption_info.auxiliary_info);
	}

	return p;
}

vod_status_t
edash_packager_get_fragment_writer(
	segment_writer_t* result,
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	bool_t single_nalu_per_frame,
	segment_writer_t* segment_writer,
	const u_char* iv,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size)
{
	dash_fragment_header_extensions_t header_extensions;
	mp4_encrypt_passthrough_context_t passthrough_context;
	uint32_t media_type = media_set->sequences[0].media_type;
	vod_status_t rc;

	if (mp4_encrypt_passthrough_init(&passthrough_context, media_set->sequences))
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"edash_packager_get_fragment_writer: using encryption passthrough");

		// get the header extensions
		header_extensions.extra_traf_atoms_size = passthrough_context.total_size + ATOM_HEADER_SIZE + sizeof(senc_atom_t);
		header_extensions.write_extra_traf_atoms_callback = edash_packager_passthrough_write_encryption_atoms;
		header_extensions.write_extra_traf_atoms_context = &passthrough_context;

		// build the fragment header
		rc = dash_packager_build_fragment_header(
			request_context,
			media_set,
			segment_index,
			0,
			&header_extensions,
			size_only,
			fragment_header,
			total_fragment_size);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"edash_packager_get_fragment_writer: dash_packager_build_fragment_header failed %i", rc);
			return rc;
		}

		// use original writer
		vod_memzero(result, sizeof(*result));

		return VOD_OK;
	}

	switch (media_type)
	{
	case MEDIA_TYPE_VIDEO:
		return mp4_encrypt_video_get_fragment_writer(
			result, 
			request_context, 
			media_set, 
			segment_index, 
			single_nalu_per_frame,
			edash_packager_video_build_fragment_header,
			segment_writer, 
			iv, 
			fragment_header,
			total_fragment_size);

	case MEDIA_TYPE_AUDIO:
		rc = mp4_encrypt_audio_get_fragment_writer(
			result, 
			request_context, 
			media_set,
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
		"edash_packager_get_fragment_writer: invalid media type %uD", media_type);
	return VOD_UNEXPECTED;
}
