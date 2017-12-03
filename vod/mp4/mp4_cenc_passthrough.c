#include "mp4_cenc_passthrough.h"
#include "mp4_cenc_decrypt.h"
#include "mp4_cenc_encrypt.h"
#include "mp4_write_stream.h"
#include "../udrm.h"

bool_t
mp4_cenc_passthrough_init(mp4_cenc_passthrough_context_t* context, media_sequence_t* sequence)
{
	media_clip_filtered_t* cur_clip;
	media_track_t* first_track = sequence->filtered_clips[0].first_track;
	media_track_t* cur_track;

	context->default_auxiliary_sample_size = first_track->encryption_info.default_auxiliary_sample_size;
	context->use_subsamples = first_track->encryption_info.use_subsamples;
	context->saiz_atom_size = ATOM_HEADER_SIZE + sizeof(saiz_atom_t);
	context->auxiliary_info_size = 0;

	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		cur_track = cur_clip->first_track;

		// can only passthrough if the content is encrypted with the required key
		if (cur_track->frames.frames_source != &mp4_cenc_decrypt_frames_source ||
			vod_memcmp(
				mp4_cenc_decrypt_get_key(cur_track->frames.frames_source_context), 
				((drm_info_t*)sequence->drm_info)->key, 
				DRM_KEY_SIZE) != 0)
		{
			return FALSE;
		}

		// do not passthrough in case multiple tracks have different encryption parameters
		if (cur_track->encryption_info.default_auxiliary_sample_size != context->default_auxiliary_sample_size ||
			cur_track->encryption_info.use_subsamples != context->use_subsamples)
		{
			return FALSE;
		}

		// update sizes
		if (context->default_auxiliary_sample_size == 0)
		{
			context->saiz_atom_size += cur_track->frame_count;
		}

		context->auxiliary_info_size += cur_track->encryption_info.auxiliary_info_end - cur_track->encryption_info.auxiliary_info;
	}

	context->sequence = sequence;
	context->saio_atom_size = ATOM_HEADER_SIZE + sizeof(saio_atom_t);
	context->total_size = context->saiz_atom_size + context->saio_atom_size + context->auxiliary_info_size;

	// can use passthrough - remove the decryption frames source
	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		cur_track = cur_clip->first_track;
		mp4_cenc_decrypt_get_original_source(
			cur_track->frames.frames_source_context,
			&cur_track->frames.frames_source,
			&cur_track->frames.frames_source_context);
	}

	return TRUE;
}

u_char*
mp4_cenc_passthrough_write_saiz_saio(
	mp4_cenc_passthrough_context_t* context, 
	u_char* p, 
	size_t auxiliary_data_offset)
{
	media_clip_filtered_t* cur_clip;
	media_sequence_t* sequence = context->sequence;
	media_track_t* cur_track;

	// moof.traf.saiz
	write_atom_header(p, context->saiz_atom_size, 's', 'a', 'i', 'z');
	write_be32(p, 0);			// version, flags
	*p++ = context->default_auxiliary_sample_size;
	write_be32(p, sequence->total_frame_count);
	if (context->default_auxiliary_sample_size == 0)
	{
		for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
		{
			cur_track = cur_clip->first_track;
			p = vod_copy(p,
				cur_track->encryption_info.auxiliary_sample_sizes,
				cur_track->frame_count);
		}
	}

	// moof.traf.saio
	write_atom_header(p, context->saio_atom_size, 's', 'a', 'i', 'o');
	write_be32(p, 0);			// version, flags
	write_be32(p, 1);			// entry count
	write_be32(p, auxiliary_data_offset);

	return p;
}
