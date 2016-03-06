#include "manifest_utils.h"

vod_status_t
manifest_utils_build_request_params_string(
	request_context_t* request_context, 
	uint32_t* has_tracks,
	uint32_t segment_index,
	uint32_t sequence_index,
	uint32_t* tracks_mask,
	vod_str_t* result)
{
	u_char* p;
	size_t result_size;
	uint32_t i;

	result_size = 0;
	
	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		result_size += 1 + vod_get_int_print_len(segment_index + 1);
	}
	
	// sequence index
	if (sequence_index != INVALID_SEQUENCE_INDEX)
	{
		result_size += sizeof("-f") - 1 + vod_get_int_print_len(sequence_index + 1);
	}

	// video tracks
	if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
	{
		result_size += sizeof("-v0") - 1;
	}
	else
	{
		result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_VIDEO]) * (sizeof("-v32") - 1);
	}
	
	// audio tracks
	if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
	{
		result_size += sizeof("-a0") - 1;
	}
	else
	{
		result_size += vod_get_number_of_set_bits(tracks_mask[MEDIA_TYPE_AUDIO]) * (sizeof("-a32") - 1);
	}
	
	p = vod_alloc(request_context->pool, result_size + 1);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"manifest_utils_build_request_params_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// segment index
	if (segment_index != INVALID_SEGMENT_INDEX)
	{
		p = vod_sprintf(p, "-%uD", segment_index + 1);
	}
	
	// sequence index
	if (sequence_index != INVALID_SEQUENCE_INDEX)
	{
		p = vod_sprintf(p, "-f%uD", sequence_index + 1);
	}

	// video tracks
	if (has_tracks[MEDIA_TYPE_VIDEO])
	{
		if (tracks_mask[MEDIA_TYPE_VIDEO] == 0xffffffff)
		{
			p = vod_copy(p, "-v0", sizeof("-v0") - 1);
		}
		else
		{
			for (i = 0; i < 32; i++)
			{
				if ((tracks_mask[MEDIA_TYPE_VIDEO] & (1 << i)) == 0)
				{
					continue;
				}

				p = vod_sprintf(p, "-v%uD", i + 1);
			}
		}
	}
	
	// audio tracks
	if (has_tracks[MEDIA_TYPE_AUDIO])
	{
		if (tracks_mask[MEDIA_TYPE_AUDIO] == 0xffffffff)
		{
			p = vod_copy(p, "-a0", sizeof("-a0") - 1);
		}
		else
		{
			for (i = 0; i < 32; i++)
			{
				if ((tracks_mask[MEDIA_TYPE_AUDIO] & (1 << i)) == 0)
				{
					continue;
				}

				p = vod_sprintf(p, "-a%uD", i + 1);
			}
		}
	}
	
	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"manifest_utils_build_request_params_string: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}
