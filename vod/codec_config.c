#include "bit_read_stream.h"
#include "codec_config.h"

#define AOT_ESCAPE (31)

// TODO: implement get_nal_units for hevc

vod_status_t 
codec_config_avcc_get_nal_units(
	request_context_t* request_context,
	const u_char* extra_data,
	uint32_t extra_data_size,
	bool_t size_only,
	u_char** result,
	uint32_t* result_size)
{
	const u_char* extra_data_end = extra_data + extra_data_size;
	const u_char* cur_pos;
	u_char* sps_pps_pos;
	size_t actual_size;
	uint16_t unit_size;
	int unit_count;
	int i;

	if (extra_data_size < sizeof(avcc_config_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_avcc_get_nal_units: extra data size %uD too small", extra_data_size);
		return VOD_BAD_DATA;
	}

	// calculate total size of SPS & PPS
	*result_size = 0;
	cur_pos = extra_data + sizeof(avcc_config_t);
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

			unit_size = parse_be16(cur_pos);
			cur_pos += sizeof(uint16_t);
			if (cur_pos + unit_size > extra_data_end)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"codec_config_avcc_get_nal_units: unit size %uD overflows the extra data buffer", (uint32_t)unit_size);
				return VOD_BAD_DATA;
			}

			cur_pos += unit_size;
			*result_size += sizeof(uint32_t) + unit_size;
		}
	}

	if (size_only)
	{
		*result = NULL;
		return VOD_OK;
	}

	// allocate buffer
	*result = vod_alloc(request_context->pool, *result_size);
	if (*result == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"codec_config_avcc_get_nal_units: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	sps_pps_pos = *result;

	// copy data
	cur_pos = extra_data + sizeof(avcc_config_t);
	for (i = 0; i < 2; i++)		// once for SPS, once for PPS
	{
		for (unit_count = *cur_pos++ & 0x1f; unit_count; unit_count--)
		{
			unit_size = parse_be16(cur_pos);
			cur_pos += sizeof(uint16_t);

			*((uint32_t*)sps_pps_pos) = 0x01000000;
			sps_pps_pos += sizeof(uint32_t);

			vod_memcpy(sps_pps_pos, cur_pos, unit_size);
			cur_pos += unit_size;
			sps_pps_pos += unit_size;
		}
	}

	actual_size = sps_pps_pos - *result;
	if (actual_size != *result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_avcc_get_nal_units: actual sps/pps size %uz is different than calculated size %uD",
			actual_size, *result_size);
		return VOD_UNEXPECTED;
	}

	vod_log_buffer(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "codec_config_avcc_get_nal_units: parsed extra data ", *result, *result_size);
	return VOD_OK;
}

// Note: taken from gf_odf_hevc_cfg_read_bs in GPAC code
static vod_status_t 
codec_config_hevc_config_parse(request_context_t* request_context, const u_char* buffer, int size, hevc_config_t* cfg)
{
	bit_reader_state_t reader;

	bit_read_stream_init(&reader, buffer, size);

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

	return VOD_OK;
}

static vod_status_t
codec_config_get_avc_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	avcc_config_t* config;
	u_char* p;

	if (media_info->extra_data_size < sizeof(avcc_config_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"codec_config_get_avc_codec_name: extra data too small");
		return VOD_BAD_DATA;
	}

	config = (avcc_config_t*)media_info->extra_data;

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

	rc = codec_config_hevc_config_parse(request_context, media_info->extra_data, media_info->extra_data_size, &cfg);
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
	switch (media_info->format)
	{
	case FORMAT_AVC1:
	case FORMAT_h264:
	case FORMAT_H264:
		return codec_config_get_avc_codec_name(request_context, media_info);

	case FORMAT_HEV1:
	case FORMAT_HVC1:
		return codec_config_get_hevc_codec_name(request_context, media_info);

	default:
		return VOD_UNEXPECTED;
	}
}

vod_status_t
codec_config_get_audio_codec_name(request_context_t* request_context, media_info_t* media_info)
{
	u_char* p;

	// Note: currently only mp4a is supported
	if (media_info->extra_data_size > 0)
	{
		p = vod_sprintf(media_info->codec_name.data, "%*s.%02uxD.%01uD",
			(size_t)sizeof(uint32_t),
			&media_info->format,
			(uint32_t)media_info->u.audio.object_type_id,
			(uint32_t)(media_info->extra_data[0] & 0xF8) >> 3);
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
codec_config_mp4a_config_parse(
	request_context_t* request_context, 
	const u_char* buffer, 
	int size, 
	mp4a_config_t* result)
{
	bit_reader_state_t reader;

	vod_log_buffer(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, "codec_config_mp4a_config_parse: extra data ", buffer, size);

	bit_read_stream_init(&reader, buffer, size);

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

// AVC SPS parsing code draft - currently disabled
// can be used to optimize the calculation of the bitrate, in case HRD parameters are present
#if 0

#define EXTENDED_SAR (255)

static vod_inline void
bit_read_stream_skip_exp(bit_reader_state_t* reader)
{
	int zero_count;

	for (zero_count = 0; bit_read_stream_get_one(reader) == 0 && !reader->stream.eof_reached; zero_count++);

	bit_read_stream_skip(reader, zero_count);
}

static vod_inline uint32_t 
bit_read_stream_get_unsigned_exp(bit_reader_state_t* reader)
{
	int zero_count;

	for (zero_count = 0; bit_read_stream_get_one(reader) == 0 && !reader->stream.eof_reached; zero_count++);

	return (1 << zero_count) - 1 + bit_read_stream_get(reader, zero_count);
}

static vod_inline int32_t
bit_read_stream_get_signed_exp(bit_reader_state_t* reader)
{
	int32_t value = bit_read_stream_get_unsigned_exp(reader);
	if (value > 0)
	{
		if (value & 1)		// positive
		{
			value = (value + 1) / 2;
		}
		else
		{
			value = -(value / 2);
		}
	}
	return value;
}

static void 
parse_avc_hrd_parameters(bit_reader_state_t* reader)
{
	uint32_t cpb_cnt_minus1;
	uint32_t i;

	cpb_cnt_minus1 = bit_read_stream_get_unsigned_exp(reader); // cpb_cnt_minus1
	bit_read_stream_get(reader, 4); // bit_rate_scale
	bit_read_stream_get(reader, 4); // cpb_size_scale
	for (i = 0; i < cpb_cnt_minus1 + 1; i++)
	{
		bit_read_stream_get_unsigned_exp(reader); // bit_rate_value_minus1_SchedSelIdx_
		bit_read_stream_get_unsigned_exp(reader); // cpb_size_value_minus1_SchedSelIdx_
		bit_read_stream_get_one(reader); // cbr_flag_SchedSelIdx_
	}
	bit_read_stream_get(reader, 5); // initial_cpb_removal_delay_length_minus1
	bit_read_stream_get(reader, 5); // cpb_removal_delay_length_minus1
	bit_read_stream_get(reader, 5); // dpb_output_delay_length_minus1
	bit_read_stream_get(reader, 5); // time_offset_length
}

static void 
parse_avc_vui_parameters(bit_reader_state_t* reader)
{
	uint32_t aspect_ratio_info_present_flag;
	uint32_t aspect_ratio_idc;
	uint32_t overscan_info_present_flag;
	uint32_t video_signal_type_present_flag;
	uint32_t colour_description_present_flag;
	uint32_t chroma_loc_info_present_flag;
	uint32_t timing_info_present_flag;
	uint32_t nal_hrd_parameters_present_flag;
	uint32_t vcl_hrd_parameters_present_flag;
	uint32_t bitstream_restriction_flag;

	aspect_ratio_info_present_flag = bit_read_stream_get_one(reader); // aspect_ratio_info_present_flag
	if (aspect_ratio_info_present_flag)
	{
		aspect_ratio_idc = bit_read_stream_get(reader, 8); // aspect_ratio_idc
		if (aspect_ratio_idc == EXTENDED_SAR)
		{
			bit_read_stream_get(reader, 16); // sar_width
			bit_read_stream_get(reader, 16); // sar_height
		}
	}
	overscan_info_present_flag = bit_read_stream_get_one(reader); // overscan_info_present_flag
	if (overscan_info_present_flag)
	{
		bit_read_stream_get_one(reader); // overscan_appropriate_flag
	}
	video_signal_type_present_flag = bit_read_stream_get_one(reader); // video_signal_type_present_flag
	if (video_signal_type_present_flag)
	{
		bit_read_stream_get(reader, 3); // video_format
		bit_read_stream_get_one(reader); // video_full_range_flag
		colour_description_present_flag = bit_read_stream_get_one(reader); // colour_description_present_flag
		if (colour_description_present_flag)
		{
			bit_read_stream_get(reader, 8); // colour_primaries
			bit_read_stream_get(reader, 8); // transfer_characteristics
			bit_read_stream_get(reader, 8); // matrix_coefficients
		}
	}
	chroma_loc_info_present_flag = bit_read_stream_get_one(reader); // chroma_loc_info_present_flag
	if (chroma_loc_info_present_flag)
	{
		bit_read_stream_get_unsigned_exp(reader); // chroma_sample_loc_type_top_field
		bit_read_stream_get_unsigned_exp(reader); // chroma_sample_loc_type_bottom_field
	}
	timing_info_present_flag = bit_read_stream_get_one(reader); // timing_info_present_flag
	if (timing_info_present_flag)
	{
		bit_read_stream_get(reader, 32); // num_units_in_tick
		bit_read_stream_get(reader, 32); // time_scale
		bit_read_stream_get_one(reader); // fixed_frame_rate_flag
	}
	nal_hrd_parameters_present_flag = bit_read_stream_get_one(reader); // nal_hrd_parameters_present_flag
	if (nal_hrd_parameters_present_flag)
	{
		parse_avc_hrd_parameters(reader);
	}
	vcl_hrd_parameters_present_flag = bit_read_stream_get_one(reader); // vcl_hrd_parameters_present_flag
	if (vcl_hrd_parameters_present_flag)
	{
		parse_avc_hrd_parameters(reader);
	}
	if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag)
	{
		bit_read_stream_get_one(reader); // low_delay_hrd_flag
	}
	bit_read_stream_get_one(reader); // pic_struct_present_flag
	bitstream_restriction_flag = bit_read_stream_get_one(reader); // bitstream_restriction_flag
	if (bitstream_restriction_flag)
	{
		bit_read_stream_get_one(reader); // motion_vectors_over_pic_boundaries_flag
		bit_read_stream_get_unsigned_exp(reader); // max_bytes_per_pic_denom
		bit_read_stream_get_unsigned_exp(reader); // max_bits_per_mb_denom
		bit_read_stream_get_unsigned_exp(reader); // log2_max_mv_length_horizontal
		bit_read_stream_get_unsigned_exp(reader); // log2_max_mv_length_vertical
		bit_read_stream_get_unsigned_exp(reader); // num_reorder_frames
		bit_read_stream_get_unsigned_exp(reader); // max_dec_frame_buffering
	}
}

static bool_t 
parse_avc_rbsp_trailing_bits(bit_reader_state_t* reader)
{
	uint32_t one_bit;

	if (reader->stream.eof_reached)
	{
		return FALSE;
	}

	one_bit = bit_read_stream_get_one(reader);
	if (reader->stream.eof_reached)
	{
		return TRUE;
	}

	if (one_bit != 1)
	{
		return FALSE;
	}

	while (!reader->stream.eof_reached)
	{
		if (bit_read_stream_get_one(reader) != 0)
		{
			return FALSE;
		}
	}

	return TRUE;
}

static bool_t 
parse_avc_sps(const u_char* buffer, int buffer_size)
{
	bit_reader_state_t reader;
	uint32_t profile_idc;
	uint32_t chroma_format_idc;
	uint32_t pic_order_cnt_type;
	uint32_t num_ref_frames_in_pic_order_cnt_cycle;
	uint32_t frame_mbs_only_flag;
	uint32_t frame_cropping_flag;
	uint32_t vui_parameters_present_flag;
	uint32_t profile_idc;
	uint32_t i;

	bit_read_stream_init(&reader, buffer, buffer_size);

	profile_idc = bit_read_stream_get(&reader, 8); // profile_idc
	bit_read_stream_get_one(&reader); // constraint_set0_flag
	bit_read_stream_get_one(&reader); // constraint_set1_flag
	bit_read_stream_get_one(&reader); // constraint_set2_flag
	bit_read_stream_get(&reader, 5); // reserved_zero_5bits
	bit_read_stream_get(&reader, 8); // level_idc
	bit_read_stream_get_unsigned_exp(&reader); // seq_parameter_set_id

	switch (profile_idc)
	{
	case 100: // High profile
	case 110: // High10 profile
	case 122: // High422 profile
	case 244: // High444 Predictive profile
	case  44: // Cavlc444 profile
	case  83: // Scalable Constrained High profile (SVC)
	case  86: // Scalable High Intra profile (SVC)
	case 118: // Stereo High profile (MVC)
	case 128: // Multiview High profile (MVC)
	case 138: // Multiview Depth High profile (MVCD)
	case 144: // old High444 profile
		chroma_format_idc = bit_read_stream_get_unsigned_exp(&reader);	// chroma_format_idc
		if (chroma_format_idc == 3) {
			bit_read_stream_get_one(&reader);	// residual_color_transform_flag
		}
		bit_read_stream_get_unsigned_exp(&reader); // bit_depth_luma
		bit_read_stream_get_unsigned_exp(&reader); // bit_depth_chroma
		bit_read_stream_get_one(&reader);	// transform_bypass
		break;
	}

	bit_read_stream_get_unsigned_exp(&reader); // log2_max_frame_num_minus4
	pic_order_cnt_type = bit_read_stream_get_unsigned_exp(&reader);
	switch (pic_order_cnt_type)
	{
	case 0:
		bit_read_stream_get_unsigned_exp(&reader); // log2_max_pic_order_cnt_lsb_minus4
		break;

	case 1:
		bit_read_stream_get_one(&reader);	// delta_pic_order_always_zero_flag
		bit_read_stream_get_signed_exp(&reader);	// offset_for_non_ref_pic
		bit_read_stream_get_signed_exp(&reader);	// offset_for_top_to_bottom_field
		num_ref_frames_in_pic_order_cnt_cycle = bit_read_stream_get_unsigned_exp(&reader);	// num_ref_frames_in_pic_order_cnt_cycle
		for (i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++)
		{
			bit_read_stream_get_signed_exp(&reader);	// offset_for_ref_frame[i]
		}
		break;
	}
	bit_read_stream_get_unsigned_exp(&reader);	// num_ref_frames
	bit_read_stream_get_one(&reader);	// gaps_in_frame_num_value_allowed_flag
	bit_read_stream_get_unsigned_exp(&reader);	// pic_width_in_mbs_minus1
	bit_read_stream_get_unsigned_exp(&reader);	// pic_height_in_map_units_minus1
	frame_mbs_only_flag = bit_read_stream_get_one(&reader);	// frame_mbs_only_flag
	if (!frame_mbs_only_flag)
	{
		bit_read_stream_get_one(&reader);	// mb_adaptive_frame_field_flag
	}
	bit_read_stream_get_one(&reader);	// direct_8x8_inference_flag
	frame_cropping_flag = bit_read_stream_get_one(&reader);	// frame_cropping_flag
	if (frame_cropping_flag)
	{
		bit_read_stream_get_unsigned_exp(&reader);	// frame_crop_left_offset
		bit_read_stream_get_unsigned_exp(&reader);	// frame_crop_right_offset
		bit_read_stream_get_unsigned_exp(&reader);	// frame_crop_top_offset
		bit_read_stream_get_unsigned_exp(&reader);	// frame_crop_bottom_offset
	}
	vui_parameters_present_flag = bit_read_stream_get_one(&reader);	// vui_parameters_present_flag
	if (vui_parameters_present_flag)
	{
		parse_avc_vui_parameters(&reader);
	}
	return parse_avc_rbsp_trailing_bits(&reader);
}

vod_status_t
avcc_config_parse_sps(
	const u_char* extra_data,
	uint32_t extra_data_size)
{
	const u_char* extra_data_end = extra_data + extra_data_size;
	const u_char* cur_pos;
	uint16_t unit_size;
	int unit_count;

	if (extra_data_size < sizeof(avcc_config_t) + sizeof(uint8_t))
	{
	}

	cur_pos = extra_data + sizeof(avcc_config_t);

	for (unit_count = (*cur_pos++ & 0x1f); unit_count; unit_count--)
	{
		if (cur_pos + sizeof(uint16_t) > extra_data_end)
		{
		}

		unit_size = parse_be16(cur_pos);
		cur_pos += sizeof(uint16_t);
		if (cur_pos + unit_size > extra_data_end)
		{
		}

		if (unit_size == 0)
		{
		}

		// skip the nal unit type
		cur_pos++;
		unit_size--;

		parse_avc_sps(cur_pos, unit_size);

		cur_pos += unit_size;
	}
}

#endif
