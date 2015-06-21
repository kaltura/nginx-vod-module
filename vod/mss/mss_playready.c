#include "mss_playready.h"
#include "mss_packager.h"
#include "../mp4/mp4_encrypt.h"

// manifest constants
#define VOD_MSS_PLAYREADY_PROTECTION_PREFIX						\
	"  <Protection>\n"

#define VOD_MSS_PLAYREADY_PROTECTION_HEADER_PREFIX				\
	"     <ProtectionHeader SystemID=\""

#define VOD_MSS_PLAYREADY_PROTECTION_HEADER_DELIMITER			\
	"\">"

#define VOD_MSS_PLAYREADY_PROTECTION_HEADER_SUFFIX				\
	"</ProtectionHeader>\n"

#define VOD_MSS_PLAYREADY_PROTECTION_SUFFIX						\
	"  </Protection>\n"

// typedefs
typedef struct {
	u_char uuid[16];
	u_char version[1];
	u_char flags[3];
	u_char entries[4];
} uuid_piff_atom_t;

typedef struct
{
	mp4_encrypt_state_t* state;
	size_t uuid_piff_atom_size;
} mss_playready_audio_extra_traf_atoms_context;

typedef struct
{
	mp4_encrypt_video_state_t* state;
	size_t uuid_piff_atom_size;
} mss_playready_video_extra_traf_atoms_context;

// constants
static const uint8_t piff_uuid[] = {
	0xa2, 0x39, 0x4f, 0x52, 0x5a, 0x9b, 0x4f, 0x14, 
	0xa2, 0x44, 0x6c, 0x42, 0x7c, 0x64, 0x8d, 0xf4
};

static u_char*
mss_playready_write_protection_tag(void* context, u_char* p, mpeg_metadata_t* mpeg_metadata)
{
	// XXXX taking only the first stream
	mp4_encrypt_info_t* drm_info = (mp4_encrypt_info_t*)mpeg_metadata->first_stream->file_info.drm_info;
	mp4_encrypt_system_info_t* cur_info;
	vod_str_t base64;

	p = vod_copy(p, VOD_MSS_PLAYREADY_PROTECTION_PREFIX, sizeof(VOD_MSS_PLAYREADY_PROTECTION_PREFIX) - 1);
	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		p = vod_copy(p, VOD_MSS_PLAYREADY_PROTECTION_HEADER_PREFIX, sizeof(VOD_MSS_PLAYREADY_PROTECTION_HEADER_PREFIX) - 1);
		p = mp4_encrypt_write_guid(p, cur_info->system_id);
		p = vod_copy(p, VOD_MSS_PLAYREADY_PROTECTION_HEADER_DELIMITER, sizeof(VOD_MSS_PLAYREADY_PROTECTION_HEADER_DELIMITER) - 1);
		base64.data = p;
		vod_encode_base64(&base64, &cur_info->data);
		p += base64.len;
		p = vod_copy(p, VOD_MSS_PLAYREADY_PROTECTION_HEADER_SUFFIX, sizeof(VOD_MSS_PLAYREADY_PROTECTION_HEADER_SUFFIX) - 1);
	}
	p = vod_copy(p, VOD_MSS_PLAYREADY_PROTECTION_SUFFIX, sizeof(VOD_MSS_PLAYREADY_PROTECTION_SUFFIX) - 1);
	return p;
}

vod_status_t
mss_playready_build_manifest(
	request_context_t* request_context,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	// XXXX taking only the first stream
	mp4_encrypt_info_t* drm_info = (mp4_encrypt_info_t*)mpeg_metadata->first_stream->file_info.drm_info;
	mp4_encrypt_system_info_t* cur_info;
	size_t extra_tags_size;

	extra_tags_size = sizeof(VOD_MSS_PLAYREADY_PROTECTION_PREFIX) - 1 + sizeof(VOD_MSS_PLAYREADY_PROTECTION_SUFFIX) - 1;
	for (cur_info = drm_info->pssh_array.first; cur_info < drm_info->pssh_array.last; cur_info++)
	{
		extra_tags_size +=
			sizeof(VOD_MSS_PLAYREADY_PROTECTION_HEADER_PREFIX) - 1 +
			VOD_GUID_LENGTH + 
			sizeof(VOD_MSS_PLAYREADY_PROTECTION_HEADER_DELIMITER) - 1 +
			vod_base64_encoded_length(cur_info->data.len) + 
			sizeof(VOD_MSS_PLAYREADY_PROTECTION_HEADER_SUFFIX) - 1;
	}

	return mss_packager_build_manifest(
		request_context,
		segmenter_conf,
		mpeg_metadata,
		extra_tags_size,
		mss_playready_write_protection_tag,
		NULL,
		result);
}

static u_char*
mss_playready_audio_write_uuid_piff_atom(u_char* p, mp4_encrypt_state_t* state, mpeg_stream_metadata_t* stream_metadata, size_t atom_size)
{
	write_atom_header(p, atom_size, 'u', 'u', 'i', 'd');
	p = vod_copy(p, piff_uuid, sizeof(piff_uuid));
	write_dword(p, 0);
	write_dword(p, stream_metadata->frame_count);
	p = mp4_encrypt_audio_write_auxiliary_data(state, p);

	return p;
}

static u_char*
mss_playready_audio_write_extra_traf_atoms(void* ctx, u_char* p, size_t mdat_atom_start)
{
	mss_playready_audio_extra_traf_atoms_context* context = (mss_playready_audio_extra_traf_atoms_context*)ctx;
	size_t auxiliary_data_start;

	auxiliary_data_start = mdat_atom_start -
		(ATOM_HEADER_SIZE +				// mdat
		mp4_encrypt_audio_get_auxiliary_data_size(context->state) +
		context->state->saiz_atom_size +
		context->state->saio_atom_size);

	p = mss_playready_audio_write_uuid_piff_atom(p, context->state, context->state->stream_metadata, context->uuid_piff_atom_size);
	p = mp4_encrypt_audio_write_saiz_saio(context->state, p, auxiliary_data_start);
	return p;
}

static u_char*
mss_playready_video_write_uuid_piff_atom(u_char* p, mp4_encrypt_video_state_t* state, mpeg_stream_metadata_t* stream_metadata, size_t atom_size)
{
	write_atom_header(p, atom_size, 'u', 'u', 'i', 'd');
	p = vod_copy(p, piff_uuid, sizeof(piff_uuid));
	write_dword(p, 2);
	write_dword(p, stream_metadata->frame_count);
	p = vod_copy(p, state->auxiliary_data.start, state->auxiliary_data.pos - state->auxiliary_data.start);

	return p;
}

static u_char*
mss_playready_video_write_extra_traf_atoms(void* ctx, u_char* p, size_t mdat_atom_start)
{
	mss_playready_video_extra_traf_atoms_context* context = (mss_playready_video_extra_traf_atoms_context*)ctx;
	size_t auxiliary_data_start;

	auxiliary_data_start = mdat_atom_start -
		(ATOM_HEADER_SIZE +				// mdat
		context->state->auxiliary_data.pos - context->state->auxiliary_data.start +
		context->state->base.saiz_atom_size +
		context->state->base.saio_atom_size);

	p = mss_playready_video_write_uuid_piff_atom(p, context->state, context->state->base.stream_metadata, context->uuid_piff_atom_size);
	p = mp4_encrypt_video_write_saiz_saio(context->state, p, auxiliary_data_start);
	return p;
}

static vod_status_t
mss_playready_audio_build_fragment_header(
	mp4_encrypt_state_t* state,
	bool_t size_only,
	vod_str_t* fragment_header,
	size_t* total_fragment_size)
{
	vod_status_t rc;
	mss_playready_audio_extra_traf_atoms_context writer_context;

	writer_context.uuid_piff_atom_size = ATOM_HEADER_SIZE + sizeof(uuid_piff_atom_t) + mp4_encrypt_audio_get_auxiliary_data_size(state);
	writer_context.state = state;

	rc = mss_packager_build_fragment_header(
		state->request_context,
		state->stream_metadata,
		state->segment_index,
		writer_context.uuid_piff_atom_size + state->saiz_atom_size + state->saio_atom_size,
		mss_playready_audio_write_extra_traf_atoms,
		&writer_context,
		size_only,
		fragment_header,
		total_fragment_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"mss_playready_audio_build_fragment_header: mss_packager_build_fragment_header failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

static vod_status_t
mss_playready_video_write_fragment_header(mp4_encrypt_video_state_t* state)
{
	mss_playready_video_extra_traf_atoms_context writer_context;
	vod_str_t fragment_header;
	size_t total_fragment_size;
	bool_t reuse_buffer;
	vod_status_t rc;

	writer_context.uuid_piff_atom_size = ATOM_HEADER_SIZE + sizeof(uuid_piff_atom_t) + state->auxiliary_data.pos - state->auxiliary_data.start;
	writer_context.state = state;

	rc = mss_packager_build_fragment_header(
		state->base.request_context,
		state->base.stream_metadata,
		state->base.segment_index,
		writer_context.uuid_piff_atom_size + state->base.saiz_atom_size + state->base.saio_atom_size,
		mss_playready_video_write_extra_traf_atoms,
		&writer_context,
		FALSE,
		&fragment_header,
		&total_fragment_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->base.request_context->log, 0,
			"mss_playready_video_write_fragment_header: mss_packager_build_fragment_header failed %i", rc);
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
			"mss_playready_video_write_fragment_header: write_head failed %i", rc);
		return rc;
	}

	return VOD_OK;
}

vod_status_t 
mss_playready_get_fragment_writer(
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
			mss_playready_video_write_fragment_header,
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

		rc = mss_playready_audio_build_fragment_header(
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
		"mss_playready_get_fragment_writer: invalid media type %uD", stream_metadata->media_info.media_type);
	return VOD_UNEXPECTED;
}
