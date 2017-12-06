#include "codec_config.h"
#include "media_format.h"
#include "bit_read_stream.h"

#define codec_config_copy_string(target, str)	\
	{											\
	vod_memcpy(target.data, str, sizeof(str));	\
	target.len = sizeof(str) - 1;				\
	}

#define AOT_ESCAPE (31)

vod_status_t 
codec_config_avcc_get_nal_units(
	request_context_t* request_context,
	vod_str_t* extra_data,
	bool_t size_only,
	uint32_t* nal_packet_size_length,
	vod_str_t* result)
{
	size_t extra_data_size = extra_data->len;
	const u_char* extra_data_start = extra_data->data;
	const u_char* extra_data_end = extra_data_start + extra_data_size;
	const u_char* cur_pos;
	u_char* p;
	size_t actual_size;
	uint16_t unit_size;
	int unit_count;
	int i;

	if (extra_data->len < sizeof(avcc_config_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_avcc_get_nal_units: extra data size %uz too small", extra_data->len);
		return VOD_BAD_DATA;
	}

	*nal_packet_size_length = (((avcc_config_t*)extra_data->data)->nula_length_size & 0x3) + 1;

	// calculate total size and validate
	result->len = 0;
	cur_pos = extra_data_start + sizeof(avcc_config_t);
	for (i = 0; i < 2; i++)		// once for SPS, once for PPS
	{
		if (cur_pos >= extra_data_end)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"codec_config_avcc_get_nal_units: extra data overflow while reading unit count");
			return VOD_BAD_DATA;
		}

		for (unit_count = (*cur_pos++ & 0x1f); unit_count; unit_count--)
		{
			if (cur_pos + sizeof(uint16_t) > extra_data_end)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"codec_config_avcc_get_nal_units: extra data overflow while reading unit size");
				return VOD_BAD_DATA;
			}

			read_be16(cur_pos, unit_size);
			if (cur_pos + unit_size > extra_data_end)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"codec_config_avcc_get_nal_units: unit size %uD overflows the extra data buffer", (uint32_t)unit_size);
				return VOD_BAD_DATA;
			}

			cur_pos += unit_size;
			result->len += sizeof(uint32_t) + unit_size;
		}
	}

	if (size_only)
	{
		result->data = NULL;
		return VOD_OK;
	}

	// allocate buffer
	p = vod_alloc(request_context->pool, result->len);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"codec_config_avcc_get_nal_units: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// copy data
	cur_pos = extra_data_start + sizeof(avcc_config_t);
	for (i = 0; i < 2; i++)		// once for SPS, once for PPS
	{
		for (unit_count = *cur_pos++ & 0x1f; unit_count; unit_count--)
		{
			unit_size = parse_be16(cur_pos);
			cur_pos += sizeof(uint16_t);

			*((uint32_t*)p) = 0x01000000;
			p += sizeof(uint32_t);

			vod_memcpy(p, cur_pos, unit_size);
			cur_pos += unit_size;
			p += unit_size;
		}
	}

	actual_size = p - result->data;
	if (actual_size != result->len)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_avcc_get_nal_units: actual extra data size %uz is different than calculated size %uz",
			actual_size, result->len);
		return VOD_UNEXPECTED;
	}

	vod_log_buffer(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "codec_config_avcc_get_nal_units: parsed extra data ", result->data, result->len);
	return VOD_OK;
}

// Note: taken from gf_odf_hevc_cfg_read_bs in GPAC code
vod_status_t 
codec_config_hevc_config_parse(
	request_context_t* request_context, 
	vod_str_t* extra_data, 
	hevc_config_t* cfg, 
	const u_char** end_pos)
{
	bit_reader_state_t reader;

	bit_read_stream_init(&reader, extra_data->data, extra_data->len);

	cfg->configurationVersion = bit_read_stream_get(&reader, 8);
	cfg->profile_space = bit_read_stream_get(&reader, 2);
	cfg->tier_flag = bit_read_stream_get_one(&reader);
	cfg->profile_idc = bit_read_stream_get(&reader, 5);
	cfg->general_profile_compatibility_flags = bit_read_stream_get(&reader, 32);

	cfg->progressive_source_flag = bit_read_stream_get_one(&reader);
	cfg->interlaced_source_flag = bit_read_stream_get_one(&reader);
	cfg->non_packed_constraint_flag = bit_read_stream_get_one(&reader);
	cfg->frame_only_constraint_flag = bit_read_stream_get_one(&reader);
	/*only lowest 44 bits used*/
	cfg->constraint_indicator_flags = bit_read_stream_get_long(&reader, 44);
	cfg->level_idc = bit_read_stream_get(&reader, 8);
	bit_read_stream_get(&reader, 4);
	cfg->min_spatial_segmentation_idc = bit_read_stream_get(&reader, 12);

	bit_read_stream_get(&reader, 6);
	cfg->parallelismType = bit_read_stream_get(&reader, 2);

	bit_read_stream_get(&reader, 6);
	cfg->chromaFormat = bit_read_stream_get(&reader, 2);
	bit_read_stream_get(&reader, 5);
	cfg->luma_bit_depth = bit_read_stream_get(&reader, 3) + 8;
	bit_read_stream_get(&reader, 5);
	cfg->chroma_bit_depth = bit_read_stream_get(&reader, 3) + 8;
	cfg->avgFrameRate = bit_read_stream_get(&reader, 16);
	cfg->constantFrameRate = bit_read_stream_get(&reader, 2);
	cfg->numTemporalLayers = bit_read_stream_get(&reader, 3);
	cfg->temporalIdNested = bit_read_stream_get_one(&reader);

	cfg->nal_unit_size = 1 + bit_read_stream_get(&reader, 2);

	/* TODO: update this
	if (is_shvc) {
		cfg->is_shvc = 1;
		cfg->complete_representation = bit_read_stream_get_one(&reader);
		cfg->non_hevc_base_layer = bit_read_stream_get_one(&reader);
		cfg->num_layers = 1 + bit_read_stream_get(&reader, 6);
		cfg->scalability_mask = bit_read_stream_get(&reader, 16);
	}*/

	if (reader.stream.eof_reached)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_hevc_config_parse: overflow while parsing hevc config");
		return VOD_BAD_DATA;
	}

	if (end_pos != NULL)
	{
		*end_pos = reader.stream.cur_pos + (reader.cur_bit >= 0 ? 1 : 0);
	}

	return VOD_OK;
}

vod_status_t
codec_config_hevc_get_nal_units(
	request_context_t* request_context,
	vod_str_t* extra_data,
	bool_t size_only,
	uint32_t* nal_packet_size_length,
	vod_str_t* result)
{
	hevc_config_t cfg;
	vod_status_t rc;
	const u_char* start_pos;
	const u_char* cur_pos;
	const u_char* end_pos;
	size_t actual_size;
	uint16_t unit_size;
	uint16_t count;
	uint8_t type_count;
	u_char* p;

	rc = codec_config_hevc_config_parse(request_context, extra_data, &cfg, &start_pos);
	if (rc != VOD_OK)
	{
		return rc;
	}

	*nal_packet_size_length = cfg.nal_unit_size;

	end_pos = extra_data->data + extra_data->len;

	// calculate total size and validate
	result->len = 0;
	cur_pos = start_pos;
	if (cur_pos >= end_pos)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_hevc_get_nal_units: extra data overflow while reading type count");
		return VOD_BAD_DATA;
	}

	for (type_count = *cur_pos++; type_count > 0; type_count--)
	{
		if (cur_pos + 3 > end_pos)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"codec_config_hevc_get_nal_units: extra data overflow while reading type header");
			return VOD_BAD_DATA;
		}

		cur_pos++;
		read_be16(cur_pos, count);

		for (; count > 0; count--)
		{
			if (cur_pos + sizeof(uint16_t) > end_pos)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"codec_config_hevc_get_nal_units: extra data overflow while reading unit size");
				return VOD_BAD_DATA;
			}

			read_be16(cur_pos, unit_size);

			cur_pos += unit_size;

			if (cur_pos > end_pos)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"codec_config_hevc_get_nal_units: extra data overflow while reading unit data");
				return VOD_BAD_DATA;
			}

			result->len += sizeof(uint32_t) + unit_size;
		}
	}

	if (size_only)
	{
		result->data = NULL;
		return VOD_OK;
	}

	// allocate buffer
	p = vod_alloc(request_context->pool, result->len);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"codec_config_hevc_get_nal_units: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// copy data
	cur_pos = start_pos;
	for (type_count = *cur_pos++; type_count > 0; type_count--)
	{
		cur_pos++;		// unit type
		read_be16(cur_pos, count);

		for (; count > 0; count--)
		{
			read_be16(cur_pos, unit_size);

			*((uint32_t*)p) = 0x01000000;
			p += sizeof(uint32_t);

			vod_memcpy(p, cur_pos, unit_size);
			p += unit_size;

			cur_pos += unit_size;
		}
	}

	// verify size
	actual_size = p - result->data;
	if (actual_size != result->len)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_hevc_get_nal_units: actual extra data size %uz is different than calculated size %uz",
			actual_size, result->len);
		return VOD_UNEXPECTED;
	}

	vod_log_buffer(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "codec_config_hevc_get_nal_units: parsed extra data ", result->data, result->len);
	return VOD_OK;
}

static vod_status_t
codec_config_get_avc_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	avcc_config_t* config;
	u_char* p;

	if (media_info->extra_data.len < sizeof(avcc_config_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_get_avc_codec_name: extra data too small");
		return VOD_BAD_DATA;
	}

	config = (avcc_config_t*)media_info->extra_data.data;

	p = vod_sprintf(media_info->codec_name.data, "%*s.%02uxD%02uxD%02uxD",
		(size_t)sizeof(uint32_t),
		&media_info->format,
		(uint32_t)config->profile,
		(uint32_t)config->compatibility,
		(uint32_t)config->level);

	media_info->codec_name.len = p - media_info->codec_name.data;

	return VOD_OK;
}

static uint32_t
codec_config_flip_bits_32(uint32_t n)
{
	uint32_t i, res = 0;

	for (i = 0; i < 32; i++)
	{
		res <<= 1;
		res |= n & 1;
		n >>= 1;
	}
	return res;
}

static vod_status_t
codec_config_get_hevc_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	vod_status_t rc;
	hevc_config_t cfg;
	u_char* p;
	uint8_t c;
	char profile_space[2] = { 0, 0 };
	int shift;

	rc = codec_config_hevc_config_parse(request_context, &media_info->extra_data, &cfg, NULL);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (cfg.profile_space > 0)
	{
		profile_space[0] = 'A' + (cfg.profile_space - 1);
	}

	c = cfg.progressive_source_flag << 7;
	c |= cfg.interlaced_source_flag << 6;
	c |= cfg.non_packed_constraint_flag << 5;
	c |= cfg.frame_only_constraint_flag << 4;
	c |= (cfg.constraint_indicator_flags >> 40);

	p = vod_sprintf(media_info->codec_name.data, "%*s.%s%D.%xD.%c%D.%02xD",
		(size_t)sizeof(uint32_t),
		&media_info->format,
		profile_space,
		(uint32_t)cfg.profile_idc,
		codec_config_flip_bits_32(cfg.general_profile_compatibility_flags),
		(int)(cfg.tier_flag ? 'H' : 'L'),
		(uint32_t)cfg.level_idc,
		(uint32_t)c);
	for (shift = 32; shift >= 0; shift -= 8)
	{
		if ((cfg.constraint_indicator_flags & (((uint64_t)1 << (shift + 8)) - 1)) == 0)
		{
			break;
		}
		p = vod_sprintf(p, ".%02xD", (uint32_t)((cfg.constraint_indicator_flags >> shift) & 0xFF));
	}
	*p = '\0';

	media_info->codec_name.len = p - media_info->codec_name.data;

	return VOD_OK;
}

vod_status_t
codec_config_get_video_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_AVC:
		return codec_config_get_avc_codec_name(request_context, media_info);

	case VOD_CODEC_ID_HEVC:
		return codec_config_get_hevc_codec_name(request_context, media_info);

	case VOD_CODEC_ID_VP8:
		codec_config_copy_string(media_info->codec_name, "vp8");
		return VOD_OK;

	case VOD_CODEC_ID_VP9:
		codec_config_copy_string(media_info->codec_name, "vp9");
		return VOD_OK;

	default:
		return VOD_UNEXPECTED;
	}
}

static vod_status_t
codec_config_get_mp4a_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	u_char* p;

	if (media_info->extra_data.len > 0)
	{
		p = vod_sprintf(media_info->codec_name.data, "%*s.%02uxD.%01uD",
			(size_t)sizeof(uint32_t),
			&media_info->format,
			(uint32_t)media_info->u.audio.object_type_id,
			(uint32_t)(media_info->extra_data.data[0] & 0xF8) >> 3);
	}
	else
	{
		p = vod_sprintf(media_info->codec_name.data, "%*s.%02uxD",
			(size_t)sizeof(uint32_t),
			&media_info->format,
			(uint32_t)media_info->u.audio.object_type_id);
	}

	media_info->codec_name.len = p - media_info->codec_name.data;

	return VOD_OK;
}

vod_status_t
codec_config_get_audio_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_VORBIS:
		codec_config_copy_string(media_info->codec_name, "vorbis");
		return VOD_OK;

	case VOD_CODEC_ID_OPUS:
		codec_config_copy_string(media_info->codec_name, "opus");
		return VOD_OK;

	case VOD_CODEC_ID_AC3:
		codec_config_copy_string(media_info->codec_name, "ac-3");
		return VOD_OK;

	case VOD_CODEC_ID_EAC3:
		codec_config_copy_string(media_info->codec_name, "ec-3");
		return VOD_OK;

	default:
		return codec_config_get_mp4a_codec_name(request_context, media_info);
	}
}

vod_status_t
codec_config_mp4a_config_parse(
	request_context_t* request_context, 
	vod_str_t* extra_data, 
	mp4a_config_t* result)
{
	bit_reader_state_t reader;

	vod_log_buffer(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "codec_config_mp4a_config_parse: extra data ", extra_data->data, extra_data->len);

	bit_read_stream_init(&reader, extra_data->data, extra_data->len);

	result->object_type = bit_read_stream_get(&reader, 5);
	if (result->object_type == AOT_ESCAPE)
		result->object_type = 32 + bit_read_stream_get(&reader, 6);

	result->sample_rate_index = bit_read_stream_get(&reader, 4);
	if (result->sample_rate_index == 0x0f)
		bit_read_stream_get(&reader, 24);

	result->channel_config = bit_read_stream_get(&reader, 4);

	if (reader.stream.eof_reached)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0, 
			"codec_config_mp4a_config_parse: failed to read all required audio extra data fields");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}
