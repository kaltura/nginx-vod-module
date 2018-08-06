#include "avc_hevc_parser.h"
#include "codec_config.h"
#include "avc_parser.h"
#include "avc_defs.h"

// constants
#define AVC_NAL_HEADER_SIZE (1)

// typedefs
typedef struct {
	int pic_height_in_map_units;
	int pic_width_in_mbs;
	uint8_t transfer_characteristics;
	unsigned frame_mbs_only_flag : 1;
	unsigned pic_order_cnt_type : 2;
	unsigned delta_pic_order_always_zero_flag : 1;
	unsigned log2_max_pic_order_cnt_lsb : 6;
	unsigned log2_max_frame_num : 6;
	unsigned chroma_array_type : 2;
	unsigned chroma_format_idc : 2;
	unsigned separate_colour_plane_flag : 1;
} avc_sps_t;

typedef struct {
	avc_sps_t* sps;
	int slice_group_change_rate;
	int num_ref_idx[2];
	unsigned slice_group_map_type : 3;
	unsigned num_slice_groups_minus1 : 3;
	unsigned weighted_bipred_idc : 2;
	unsigned weighted_pred_flag : 1;
	unsigned deblocking_filter_control_present_flag : 1;
	unsigned redundant_pic_cnt_present_flag : 1;
	unsigned entropy_coding_mode_flag : 1;
	unsigned bottom_field_pic_order_in_frame_present_flag : 1;
} avc_pps_t;

// enums
enum {
	AVC_SLICE_P,
	AVC_SLICE_B,
	AVC_SLICE_I,
	AVC_SLICE_SP,
	AVC_SLICE_SI,
};

// SPS
static void 
avc_parser_skip_hrd_parameters(bit_reader_state_t* reader)
{
	uint32_t cpb_cnt_minus1;
	uint32_t i;

	cpb_cnt_minus1 = bit_read_stream_get_unsigned_exp(reader); // cpb_cnt_minus1
	bit_read_stream_skip(reader, 4); // bit_rate_scale
	bit_read_stream_skip(reader, 4); // cpb_size_scale
	for (i = 0; i <= cpb_cnt_minus1 && !reader->stream.eof_reached; i++)
	{
		bit_read_stream_skip_unsigned_exp(reader); // bit_rate_value_minus1_SchedSelIdx_
		bit_read_stream_skip_unsigned_exp(reader); // cpb_size_value_minus1_SchedSelIdx_
		bit_read_stream_get_one(reader); // cbr_flag_SchedSelIdx_
	}
	bit_read_stream_skip(reader, 5); // initial_cpb_removal_delay_length_minus1
	bit_read_stream_skip(reader, 5); // cpb_removal_delay_length_minus1
	bit_read_stream_skip(reader, 5); // dpb_output_delay_length_minus1
	bit_read_stream_skip(reader, 5); // time_offset_length
}

static void 
avc_parser_parse_vui_parameters(avc_sps_t* sps, bit_reader_state_t* reader)
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
			bit_read_stream_skip(reader, 16); // sar_width
			bit_read_stream_skip(reader, 16); // sar_height
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
		bit_read_stream_skip(reader, 3); // video_format
		bit_read_stream_get_one(reader); // video_full_range_flag
		colour_description_present_flag = bit_read_stream_get_one(reader); // colour_description_present_flag
		if (colour_description_present_flag)
		{
			bit_read_stream_skip(reader, 8); // colour_primaries
			sps->transfer_characteristics = bit_read_stream_get(reader, 8);
			bit_read_stream_skip(reader, 8); // matrix_coefficients
		}
	}
	chroma_loc_info_present_flag = bit_read_stream_get_one(reader); // chroma_loc_info_present_flag
	if (chroma_loc_info_present_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader); // chroma_sample_loc_type_top_field
		bit_read_stream_skip_unsigned_exp(reader); // chroma_sample_loc_type_bottom_field
	}
	timing_info_present_flag = bit_read_stream_get_one(reader); // timing_info_present_flag
	if (timing_info_present_flag)
	{
		bit_read_stream_skip(reader, 32); // num_units_in_tick
		bit_read_stream_skip(reader, 32); // time_scale
		bit_read_stream_get_one(reader); // fixed_frame_rate_flag
	}
	nal_hrd_parameters_present_flag = bit_read_stream_get_one(reader); // nal_hrd_parameters_present_flag
	if (nal_hrd_parameters_present_flag)
	{
		avc_parser_skip_hrd_parameters(reader);
	}
	vcl_hrd_parameters_present_flag = bit_read_stream_get_one(reader); // vcl_hrd_parameters_present_flag
	if (vcl_hrd_parameters_present_flag)
	{
		avc_parser_skip_hrd_parameters(reader);
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
		bit_read_stream_skip_unsigned_exp(reader); // max_bytes_per_pic_denom
		bit_read_stream_skip_unsigned_exp(reader); // max_bits_per_mb_denom
		bit_read_stream_skip_unsigned_exp(reader); // log2_max_mv_length_horizontal
		bit_read_stream_skip_unsigned_exp(reader); // log2_max_mv_length_vertical
		bit_read_stream_skip_unsigned_exp(reader); // num_reorder_frames
		bit_read_stream_skip_unsigned_exp(reader); // max_dec_frame_buffering
	}
}

static void
avc_parser_skip_scaling_list(bit_reader_state_t* reader, int size_of_scaling_list)
{
	int last_scale = 8;
	int next_scale = 8;
	int delta_scale;
	int j;

	for (j = 0; j < size_of_scaling_list; j++) 
	{
		if (next_scale != 0) 
		{
			delta_scale = bit_read_stream_get_signed_exp(reader);
			next_scale = (last_scale + delta_scale) & 0xff;
		}
		last_scale = (next_scale == 0) ? last_scale : next_scale;
	}
}

static vod_status_t 
avc_parser_seq_parameter_set_rbsp(avc_hevc_parse_ctx_t* ctx, bit_reader_state_t* reader)
{
	avc_sps_t* sps;
	uint32_t profile_idc;
	uint32_t num_ref_frames_in_pic_order_cnt_cycle;
	uint32_t limit;
	uint32_t i;
	uint32_t seq_parameter_set_id;
	bool_t seq_scaling_matrix_present_flag;
	bool_t seq_scaling_list_present_flag;
	bool_t vui_parameters_present_flag;
	bool_t frame_cropping_flag;
	
	profile_idc = bit_read_stream_get(reader, 8); // profile_idc
	bit_read_stream_get_one(reader); // constraint_set0_flag
	bit_read_stream_get_one(reader); // constraint_set1_flag
	bit_read_stream_get_one(reader); // constraint_set2_flag
	bit_read_stream_skip(reader, 5); // reserved_zero_5bits
	bit_read_stream_skip(reader, 8); // level_idc
	seq_parameter_set_id = bit_read_stream_get_unsigned_exp(reader);

	if (seq_parameter_set_id >= MAX_SPS_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_seq_parameter_set_rbsp: invalid sps id %uD", seq_parameter_set_id);
		return VOD_BAD_DATA;
	}

	sps = avc_hevc_parser_get_ptr_array_item(&ctx->sps, seq_parameter_set_id, sizeof(*sps));
	if (sps == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->request_context->log, 0,
			"avc_parser_seq_parameter_set_rbsp: avc_hevc_parser_get_ptr_array_item failed");
		return VOD_ALLOC_FAILED;
	}

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
	case 139:
	case 134:
	case 135:
		sps->chroma_format_idc = bit_read_stream_get_unsigned_exp(reader);	// chroma_format_idc
		sps->chroma_array_type = sps->chroma_format_idc;
		if (sps->chroma_format_idc == 3)
		{
			sps->separate_colour_plane_flag = bit_read_stream_get_one(reader);
			if (sps->separate_colour_plane_flag)
			{
				sps->chroma_array_type = 0;
			}
		}
		bit_read_stream_skip_unsigned_exp(reader); // bit_depth_luma_minus8 
		bit_read_stream_skip_unsigned_exp(reader); // bit_depth_chroma_minus8
		bit_read_stream_get_one(reader);	// qpprime_y_zero_transform_bypass_flag
		seq_scaling_matrix_present_flag = bit_read_stream_get_one(reader);
		if (seq_scaling_matrix_present_flag)
		{
			limit = ((sps->chroma_format_idc != 3) ? 8 : 12);
			for (i = 0; i < limit; i++)
			{
				seq_scaling_list_present_flag = bit_read_stream_get_one(reader);
				if (seq_scaling_list_present_flag)
				{
					if (i < 6)
					{
						avc_parser_skip_scaling_list(reader, 16);
					}
					else
					{
						avc_parser_skip_scaling_list(reader, 64);
					}
				}
			}
		}
		break;

	default:
		sps->chroma_format_idc = 1;
		sps->chroma_array_type = 1;
		break;
	}

	sps->log2_max_frame_num = bit_read_stream_get_unsigned_exp(reader) + 4;
	sps->pic_order_cnt_type = bit_read_stream_get_unsigned_exp(reader);
	switch (sps->pic_order_cnt_type)
	{
	case 0:
		sps->log2_max_pic_order_cnt_lsb = bit_read_stream_get_unsigned_exp(reader) + 4;
		break;

	case 1:
		sps->delta_pic_order_always_zero_flag = bit_read_stream_get_one(reader);
		bit_read_stream_skip_signed_exp(reader);	// offset_for_non_ref_pic
		bit_read_stream_skip_signed_exp(reader);	// offset_for_top_to_bottom_field
		num_ref_frames_in_pic_order_cnt_cycle = bit_read_stream_get_unsigned_exp(reader);	// num_ref_frames_in_pic_order_cnt_cycle
		for (i = 0; i < num_ref_frames_in_pic_order_cnt_cycle && !reader->stream.eof_reached; i++)
		{
			bit_read_stream_skip_signed_exp(reader);	// offset_for_ref_frame[i]
		}
		break;
	}
	bit_read_stream_skip_unsigned_exp(reader);	// num_ref_frames
	bit_read_stream_get_one(reader);	// gaps_in_frame_num_value_allowed_flag
	sps->pic_width_in_mbs = bit_read_stream_get_unsigned_exp(reader) + 1;
	sps->pic_height_in_map_units = bit_read_stream_get_unsigned_exp(reader) + 1;
	sps->frame_mbs_only_flag = bit_read_stream_get_one(reader);
	if (!sps->frame_mbs_only_flag)
	{
		bit_read_stream_get_one(reader);	// mb_adaptive_frame_field_flag
	}
	bit_read_stream_get_one(reader);	// direct_8x8_inference_flag
	frame_cropping_flag = bit_read_stream_get_one(reader);	// frame_cropping_flag
	if (frame_cropping_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);	// frame_crop_left_offset
		bit_read_stream_skip_unsigned_exp(reader);	// frame_crop_right_offset
		bit_read_stream_skip_unsigned_exp(reader);	// frame_crop_top_offset
		bit_read_stream_skip_unsigned_exp(reader);	// frame_crop_bottom_offset
	}
	vui_parameters_present_flag = bit_read_stream_get_one(reader);	// vui_parameters_present_flag
	if (vui_parameters_present_flag)
	{
		avc_parser_parse_vui_parameters(sps, reader);
	}
	if (!avc_hevc_parser_rbsp_trailing_bits(reader))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_seq_parameter_set_rbsp: invalid trailing bits");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

// PPS
static vod_status_t
avc_parser_pic_parameter_set_rbsp(
	avc_hevc_parse_ctx_t* ctx,
	bit_reader_state_t* reader)
{
	bit_reader_state_t temp_reader;
	uint32_t seq_parameter_set_id;
	uint32_t pic_parameter_set_id;
	avc_pps_t* pps;
	unsigned pic_size_in_map_units_minus1;
	unsigned group;
	unsigned limit;
	unsigned i;
	bool_t pic_scaling_list_present_flag;
	bool_t pic_scaling_matrix_present_flag;
	bool_t transform_8x8_mode_flag;

	pic_parameter_set_id = bit_read_stream_get_unsigned_exp(reader);

	if (pic_parameter_set_id >= MAX_PPS_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_pic_parameter_set_rbsp: invalid pps id %uD", pic_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps = avc_hevc_parser_get_ptr_array_item(&ctx->pps, pic_parameter_set_id, sizeof(*pps));
	if (pps == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->request_context->log, 0,
			"avc_parser_pic_parameter_set_rbsp: avc_hevc_parser_get_ptr_array_item failed");
		return VOD_ALLOC_FAILED;
	}

	seq_parameter_set_id = bit_read_stream_get_unsigned_exp(reader);
	if (seq_parameter_set_id >= ctx->sps.nelts)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_pic_parameter_set_rbsp: invalid sps id %uD", seq_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps->sps = ((avc_sps_t**)ctx->sps.elts)[seq_parameter_set_id];
	if (pps->sps == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_pic_parameter_set_rbsp: non-existing sps id %uD", seq_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps->entropy_coding_mode_flag = bit_read_stream_get_one(reader);
	pps->bottom_field_pic_order_in_frame_present_flag = bit_read_stream_get_one(reader);
	pps->num_slice_groups_minus1 = bit_read_stream_get_unsigned_exp(reader);
	if (pps->num_slice_groups_minus1 > 0)
	{
		pps->slice_group_map_type = bit_read_stream_get_unsigned_exp(reader);
		if (pps->slice_group_map_type == 0)
		{
			for (group = 0; group <= pps->num_slice_groups_minus1 && !reader->stream.eof_reached; group++)
			{
				bit_read_stream_skip_unsigned_exp(reader);		// run_length_minus1[group]
			}
		}
		else if (pps->slice_group_map_type == 2)
		{
			for (group = 0; group < pps->num_slice_groups_minus1 && !reader->stream.eof_reached; group++)
			{
				bit_read_stream_skip_unsigned_exp(reader);		// top_left[ group ]
				bit_read_stream_skip_unsigned_exp(reader);		// bottom_right[ group ]
			}
		}
		else if (pps->slice_group_map_type == 3 || pps->slice_group_map_type == 4 || pps->slice_group_map_type == 5)
		{
			bit_read_stream_get_one(reader);					// slice_group_change_direction_flag
			pps->slice_group_change_rate = bit_read_stream_get_unsigned_exp(reader) + 1;
		}
		else if (pps->slice_group_map_type == 6)
		{
			pic_size_in_map_units_minus1 = bit_read_stream_get_unsigned_exp(reader);
			for (i = 0; i <= pic_size_in_map_units_minus1 && !reader->stream.eof_reached; i++)
			{
				bit_read_stream_skip(reader, avc_hevc_parser_ceil_log2(pps->num_slice_groups_minus1 + 1));		// slice_group_id[ i ]
			}
		}
	}

	pps->num_ref_idx[0] = bit_read_stream_get_unsigned_exp(reader) + 1;
	pps->num_ref_idx[1] = bit_read_stream_get_unsigned_exp(reader) + 1;
	pps->weighted_pred_flag = bit_read_stream_get_one(reader);
	pps->weighted_bipred_idc = bit_read_stream_get(reader, 2);
	bit_read_stream_skip_signed_exp(reader);	// pic_init_qp_minus26
	bit_read_stream_skip_signed_exp(reader);	// pic_init_qs_minus26
	bit_read_stream_skip_signed_exp(reader);	// chroma_qp_index_offset
	pps->deblocking_filter_control_present_flag = bit_read_stream_get_one(reader);
	bit_read_stream_get_one(reader);			// constrained_intra_pred_flag
	pps->redundant_pic_cnt_present_flag = bit_read_stream_get_one(reader);

	if (reader->stream.eof_reached)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_pic_parameter_set_rbsp: stream overflow");
		return VOD_BAD_DATA;
	}

	temp_reader = *reader;
	if (avc_hevc_parser_rbsp_trailing_bits(&temp_reader))	// !more_rbsp_data
	{
		return VOD_OK;
	}

	transform_8x8_mode_flag = bit_read_stream_get_one(reader);
	pic_scaling_matrix_present_flag = bit_read_stream_get_one(reader);
	if (pic_scaling_matrix_present_flag)
	{
		limit = 6 + ((pps->sps->chroma_format_idc != 3) ? 2 : 6) * transform_8x8_mode_flag;
		for (i = 0; i < limit; i++)
		{
			pic_scaling_list_present_flag = bit_read_stream_get_one(reader);
			if (pic_scaling_list_present_flag)
			{
				if (i < 6)
				{
					avc_parser_skip_scaling_list(reader, 16);
				}
				else
				{
					avc_parser_skip_scaling_list(reader, 64);
				}
			}
		}
	}

	bit_read_stream_skip_signed_exp(reader);	// second_chroma_qp_index_offset

	if (!avc_hevc_parser_rbsp_trailing_bits(reader))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_pic_parameter_set_rbsp: invalid trailing bits");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

// extra data
vod_status_t
avc_parser_parse_extra_data(
	void* context,
	vod_str_t* extra_data,
	uint32_t* nal_packet_size_length,
	uint32_t* min_packet_size)
{
	avc_hevc_parse_ctx_t* ctx = context;
	bit_reader_state_t reader;
	const u_char* extra_data_end;
	const u_char* cur_pos;
	vod_status_t rc;
	uint16_t unit_size;
	uint8_t nal_type;
	int unit_count;
	int i;

	if (extra_data->len < sizeof(avcc_config_t))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_parse_extra_data: extra data size %uz too small", extra_data->len);
		return VOD_BAD_DATA;
	}

	if (nal_packet_size_length != NULL)
	{
		*nal_packet_size_length = (((avcc_config_t*)extra_data->data)->nula_length_size & 0x3) + 1;
	}

	if (min_packet_size != NULL)
	{
		*min_packet_size = *nal_packet_size_length + AVC_NAL_HEADER_SIZE;
	}

	cur_pos = extra_data->data + sizeof(avcc_config_t);
	extra_data_end = extra_data->data + extra_data->len;

	for (i = 0; i < 2; i++)		// once for SPS, once for PPS
	{
		if (cur_pos >= extra_data_end)
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"avc_parser_parse_extra_data: extra data overflow while reading unit count");
			return VOD_BAD_DATA;
		}

		for (unit_count = (*cur_pos++ & 0x1f); unit_count; unit_count--)
		{
			if (sizeof(uint16_t) > (size_t)(extra_data_end - cur_pos))
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"avc_parser_parse_extra_data: extra data overflow while reading unit size");
				return VOD_BAD_DATA;
			}

			unit_size = parse_be16(cur_pos);
			cur_pos += sizeof(uint16_t);
			if (unit_size > extra_data_end - cur_pos)
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"avc_parser_parse_extra_data: extra data overflow while reading unit data");
				return VOD_BAD_DATA;
			}

			if (unit_size < AVC_NAL_HEADER_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"avc_parser_parse_extra_data: unit of zero size");
				return VOD_BAD_DATA;
			}

			// skip the nal unit type
			nal_type = *cur_pos;
			cur_pos += AVC_NAL_HEADER_SIZE;
			unit_size -= AVC_NAL_HEADER_SIZE;

			rc = avc_hevc_parser_emulation_prevention_decode(ctx->request_context, &reader, cur_pos, unit_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			switch (nal_type & 0x1f)
			{
			case AVC_NAL_SPS:
				rc = avc_parser_seq_parameter_set_rbsp(ctx, &reader);
				if (rc != VOD_OK)
				{
					return rc;
				}
				break;

			case AVC_NAL_PPS:
				rc = avc_parser_pic_parameter_set_rbsp(ctx, &reader);
				if (rc != VOD_OK)
				{
					return rc;
				}
				break;
			}

			cur_pos += unit_size;
		}
	}

	return VOD_OK;
}

// slice header
static void
avc_parser_skip_ref_pic_list_modification(
	bit_reader_state_t* reader,
	uint8_t slice_type)
{
	bool_t ref_pic_list_modification_flag_l0;
	bool_t ref_pic_list_modification_flag_l1;
	uint8_t modification_of_pic_nums_idc;

	slice_type %= 5;

	if (slice_type != AVC_SLICE_I && slice_type != AVC_SLICE_SI)
	{
		ref_pic_list_modification_flag_l0 = bit_read_stream_get_one(reader);
		if (ref_pic_list_modification_flag_l0)
		{
			do
			{
				modification_of_pic_nums_idc = bit_read_stream_get_unsigned_exp(reader);
				if (modification_of_pic_nums_idc == 0 ||
					modification_of_pic_nums_idc == 1)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// abs_diff_pic_num_minus1
				}
				else if (modification_of_pic_nums_idc == 2)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// long_term_pic_num
				}
			} while (modification_of_pic_nums_idc != 3 && !reader->stream.eof_reached);
		}
	}

	if (slice_type == AVC_SLICE_B)
	{
		ref_pic_list_modification_flag_l1 = bit_read_stream_get_one(reader);
		if (ref_pic_list_modification_flag_l1)
		{
			do
			{
				modification_of_pic_nums_idc = bit_read_stream_get_unsigned_exp(reader);
				if (modification_of_pic_nums_idc == 0 ||
					modification_of_pic_nums_idc == 1)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// abs_diff_pic_num_minus1
				}
				else if (modification_of_pic_nums_idc == 2)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// long_term_pic_num
				}
			} while (modification_of_pic_nums_idc != 3 && !reader->stream.eof_reached);
		}
	}
}

static void
avc_parser_skip_ref_pic_list_mvc_modification(
	bit_reader_state_t* reader,
	uint8_t slice_type)
{
	bool_t ref_pic_list_modification_flag_l0;
	bool_t ref_pic_list_modification_flag_l1;
	uint8_t modification_of_pic_nums_idc;

	slice_type %= 5;

	if (slice_type != AVC_SLICE_I && slice_type != AVC_SLICE_SI)
	{
		ref_pic_list_modification_flag_l0 = bit_read_stream_get_one(reader);
		if (ref_pic_list_modification_flag_l0)
		{
			do
			{
				modification_of_pic_nums_idc = bit_read_stream_get_unsigned_exp(reader);
				if (modification_of_pic_nums_idc == 0 ||
					modification_of_pic_nums_idc == 1)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// abs_diff_pic_num_minus1
				}
				else if (modification_of_pic_nums_idc == 2)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// long_term_pic_num
				}
				else if (modification_of_pic_nums_idc == 4 ||
					modification_of_pic_nums_idc == 5)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// abs_diff_view_idx_minus1
				}
			} while (modification_of_pic_nums_idc != 3 && !reader->stream.eof_reached);
		}
	}

	if (slice_type == AVC_SLICE_B)
	{
		ref_pic_list_modification_flag_l1 = bit_read_stream_get_one(reader);
		if (ref_pic_list_modification_flag_l1)
		{
			do
			{
				modification_of_pic_nums_idc = bit_read_stream_get_unsigned_exp(reader);
				if (modification_of_pic_nums_idc == 0 ||
					modification_of_pic_nums_idc == 1)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// abs_diff_pic_num_minus1
				}
				else if (modification_of_pic_nums_idc == 2)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// long_term_pic_num
				}
				else if (modification_of_pic_nums_idc == 4 ||
					modification_of_pic_nums_idc == 5)
				{
					bit_read_stream_skip_unsigned_exp(reader);	// abs_diff_view_idx_minus1
				}
			} while (modification_of_pic_nums_idc != 3 && !reader->stream.eof_reached);
		}
	}
}

static void
avc_parser_skip_pred_weight_table(
	bit_reader_state_t* reader,
	uint8_t slice_type,
	int* num_ref_idx,
	int chroma_array_type)
{
	bool_t luma_weight_l0_flag;
	bool_t chroma_weight_l0_flag;
	bool_t luma_weight_l1_flag;
	bool_t chroma_weight_l1_flag;
	int i, j;

	bit_read_stream_skip_unsigned_exp(reader);		// luma_log2_weight_denom
	if (chroma_array_type != 0)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// chroma_log2_weight_denom
	}
	for (i = 0; i < num_ref_idx[0] && !reader->stream.eof_reached; i++)
	{
		luma_weight_l0_flag = bit_read_stream_get_one(reader);
		if (luma_weight_l0_flag)
		{
			bit_read_stream_skip_signed_exp(reader);	// luma_weight_l0[ i ]
			bit_read_stream_skip_signed_exp(reader);	// luma_offset_l0[ i ]
		}
		if (chroma_array_type != 0)
		{
			chroma_weight_l0_flag = bit_read_stream_get_one(reader);
			if (chroma_weight_l0_flag)
			{
				for (j = 0; j < 2; j++)
				{
					bit_read_stream_skip_signed_exp(reader);	// chroma_weight_l0[ i ][ j ]
					bit_read_stream_skip_signed_exp(reader);	// chroma_offset_l0[ i ][ j ]
				}
			}
		}
	}
	if (slice_type % 5 == AVC_SLICE_B)
	{
		for (i = 0; i < num_ref_idx[1] && !reader->stream.eof_reached; i++)
		{
			luma_weight_l1_flag = bit_read_stream_get_one(reader);
			if (luma_weight_l1_flag)
			{
				bit_read_stream_skip_signed_exp(reader);	// luma_weight_l1[ i ]
				bit_read_stream_skip_signed_exp(reader);	// luma_offset_l1[ i ]
			}
			if (chroma_array_type != 0)
			{
				chroma_weight_l1_flag = bit_read_stream_get_one(reader);
				if (chroma_weight_l1_flag)
				{
					for (j = 0; j < 2; j++)
					{
						bit_read_stream_skip_signed_exp(reader);	// chroma_weight_l1[ i ][ j ]
						bit_read_stream_skip_signed_exp(reader);	// chroma_offset_l1[ i ][ j ]
					}
				}
			}
		}
	}
}

static void
avc_parser_skip_dec_ref_pic_marking(
	bit_reader_state_t* reader,
	uint8_t nal_unit_type)
{
	uint8_t memory_management_control_operation;
	bool_t adaptive_ref_pic_marking_mode_flag;

	if (nal_unit_type == AVC_NAL_IDR_SLICE)
	{
		bit_read_stream_get_one(reader);	// no_output_of_prior_pics_flag
		bit_read_stream_get_one(reader);	// long_term_reference_flag
	}
	else
	{
		adaptive_ref_pic_marking_mode_flag = bit_read_stream_get_one(reader);
		if (adaptive_ref_pic_marking_mode_flag)
		{
			do
			{
				memory_management_control_operation = bit_read_stream_get_unsigned_exp(reader);
				if (memory_management_control_operation == 1 || memory_management_control_operation == 3)
				{
					bit_read_stream_skip_unsigned_exp(reader);		// difference_of_pic_nums_minus1
				}
				if (memory_management_control_operation == 2)
				{
					bit_read_stream_skip_unsigned_exp(reader);		// long_term_pic_num
				}
				if (memory_management_control_operation == 3 || memory_management_control_operation == 6)
				{
					bit_read_stream_skip_unsigned_exp(reader);		// long_term_frame_idx
				}
				if (memory_management_control_operation == 4)
				{
					bit_read_stream_skip_unsigned_exp(reader);		// max_long_term_frame_idx_plus1
				}
			} while (memory_management_control_operation != 0 && !reader->stream.eof_reached);
		}
	}
}

vod_status_t
avc_parser_get_slice_header_size(
	void* context,
	const u_char* buffer,
	uint32_t size,
	uint32_t* result)
{
	avc_hevc_parse_ctx_t* ctx = context;
	bit_reader_state_t reader;
	const u_char* start_pos;
	vod_status_t rc;
	avc_pps_t* pps;
	avc_sps_t* sps;
	uint8_t slice_type;
	uint32_t pps_id;
	bool_t field_pic_flag = FALSE;
	uint8_t disable_deblocking_filter_idc;
	uint8_t nal_ref_idc;
	uint8_t nal_unit_type;
	bool_t num_ref_idx_active_override_flag;
	int num_ref_idx[2];
	int pic_size_in_map_units;
	int len;

	rc = avc_hevc_parser_emulation_prevention_decode(
		ctx->request_context,
		&reader,
		buffer + AVC_NAL_HEADER_SIZE,
		size - AVC_NAL_HEADER_SIZE);
	if (rc != VOD_OK)
	{
		return rc;
	}

	start_pos = reader.stream.cur_pos;

	nal_ref_idc = (buffer[0] >> 5) & 0x3;
	nal_unit_type = buffer[0] & 0x1f;

	bit_read_stream_skip_unsigned_exp(&reader); // first_mb_in_slice
	slice_type = bit_read_stream_get_unsigned_exp(&reader);

	if (slice_type > 9)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_get_slice_header_size: invalid slice type %uD", (uint32_t)slice_type);
		return VOD_BAD_DATA;
	}
	
	if (slice_type >= 5)
	{
		slice_type -= 5;
	}

	pps_id = bit_read_stream_get_unsigned_exp(&reader);
	if (pps_id >= ctx->pps.nelts)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_get_slice_header_size: invalid pps id %uD", pps_id);
		return VOD_BAD_DATA;
	}

	pps = ((avc_pps_t**)ctx->pps.elts)[pps_id];
	if (pps == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_get_slice_header_size: non-existing pps id %uD", pps_id);
		return VOD_BAD_DATA;
	}

	sps = pps->sps;

	if (sps->separate_colour_plane_flag == 1)
	{
		bit_read_stream_skip(&reader, 2);	// colour_plane_id
	}
	bit_read_stream_skip(&reader, sps->log2_max_frame_num);	// frame_num
	if (!sps->frame_mbs_only_flag)
	{
		field_pic_flag = bit_read_stream_get_one(&reader);
		if (field_pic_flag)
		{
			bit_read_stream_get_one(&reader);			// bottom_field_flag
		}
	}

	if (nal_unit_type == AVC_NAL_IDR_SLICE)
	{
		bit_read_stream_skip_unsigned_exp(&reader);		// idr_pic_id
	}

	if (sps->pic_order_cnt_type == 0) 
	{
		bit_read_stream_skip(&reader, sps->log2_max_pic_order_cnt_lsb);		// pic_order_cnt_lsb
		if (pps->bottom_field_pic_order_in_frame_present_flag && !field_pic_flag)
			bit_read_stream_skip_signed_exp(&reader);	// delta_pic_order_cnt_bottom
	}

	if (sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero_flag) 
	{
		bit_read_stream_skip_signed_exp(&reader);		// delta_pic_order_cnt[ 0 ]
		if (pps->bottom_field_pic_order_in_frame_present_flag && !field_pic_flag)
			bit_read_stream_skip_signed_exp(&reader);	// delta_pic_order_cnt[ 1 ]
	}

	if (pps->redundant_pic_cnt_present_flag)
	{
		bit_read_stream_skip_unsigned_exp(&reader);		// redundant_pic_cnt
	}

	if (slice_type == AVC_SLICE_B)
	{
		bit_read_stream_get_one(&reader);				// direct_spatial_mv_pred_flag
	}

	vod_memcpy(num_ref_idx, pps->num_ref_idx, sizeof(num_ref_idx));
	if (slice_type == AVC_SLICE_P || slice_type == AVC_SLICE_SP || slice_type == AVC_SLICE_B)
	{
		num_ref_idx_active_override_flag = bit_read_stream_get_one(&reader);
		if (num_ref_idx_active_override_flag)
		{
			num_ref_idx[0] = bit_read_stream_get_unsigned_exp(&reader) + 1;
			if (slice_type == AVC_SLICE_B)
			{
				num_ref_idx[1] = bit_read_stream_get_unsigned_exp(&reader) + 1;
			}
		}
	}

	if (nal_unit_type == 20 || nal_unit_type == 21)
	{
		avc_parser_skip_ref_pic_list_mvc_modification(&reader, slice_type);
	}
	else
	{
		avc_parser_skip_ref_pic_list_modification(&reader, slice_type);
	}
	if ((pps->weighted_pred_flag && (slice_type == AVC_SLICE_P || slice_type == AVC_SLICE_SP)) ||
		(pps->weighted_bipred_idc == 1 && slice_type == AVC_SLICE_B))
	{
		avc_parser_skip_pred_weight_table(&reader, slice_type, num_ref_idx, sps->chroma_array_type);
	}

	if (nal_ref_idc != 0)
	{
		avc_parser_skip_dec_ref_pic_marking(&reader, nal_unit_type);
	}

	if (pps->entropy_coding_mode_flag && slice_type != AVC_SLICE_I && slice_type != AVC_SLICE_SI)
	{
		bit_read_stream_skip_unsigned_exp(&reader);	// cabac_init_idc
	}

	bit_read_stream_skip_signed_exp(&reader);		// slice_qp_delta
	if (slice_type == AVC_SLICE_SP || slice_type == AVC_SLICE_SI)
	{
		if (slice_type == AVC_SLICE_SP)
		{
			bit_read_stream_get_one(&reader);		// sp_for_switch_flag
		}
		bit_read_stream_skip_signed_exp(&reader); 	// slice_qs_delta
	}

	if (pps->deblocking_filter_control_present_flag) 
	{
		disable_deblocking_filter_idc = bit_read_stream_get_unsigned_exp(&reader);
		if (disable_deblocking_filter_idc != 1) 
		{
			bit_read_stream_skip_signed_exp(&reader);	// slice_alpha_c0_offset_div2
			bit_read_stream_skip_signed_exp(&reader);	// slice_beta_offset_div2
		}
	}
	if (pps->num_slice_groups_minus1 > 0 && 
		pps->slice_group_map_type >= 3 && pps->slice_group_map_type <= 5)
	{
		pic_size_in_map_units = sps->pic_height_in_map_units * sps->pic_width_in_mbs;
		len = vod_div_ceil(pic_size_in_map_units, pps->slice_group_change_rate);
		len = avc_hevc_parser_ceil_log2(len + 1);
		bit_read_stream_skip(&reader, len);		// slice_group_change_cycle
	}
	
	if (reader.stream.eof_reached)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_get_slice_header_size: bit stream overflow");
		return VOD_BAD_DATA;
	}
	
	*result = AVC_NAL_HEADER_SIZE + (reader.stream.cur_pos - start_pos);

	if (start_pos != buffer + AVC_NAL_HEADER_SIZE)
	{
		*result += avc_hevc_parser_emulation_prevention_encode_bytes(
			start_pos, 
			reader.stream.cur_pos);
	}

	return VOD_OK;
}

vod_status_t
avc_parser_is_slice(void* context, uint8_t nal_type, bool_t* is_slice)
{
	avc_hevc_parse_ctx_t* ctx = context;

	switch (nal_type & 0x1f)
	{
	case AVC_NAL_SLICE:
	case AVC_NAL_IDR_SLICE:
		*is_slice = TRUE;
		break;

	case AVC_NAL_DPA:
	case AVC_NAL_DPB:
	case AVC_NAL_DPC:
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"avc_parser_is_slice: nal types 2-4 are not supported");
		return VOD_BAD_DATA;

	default:
		*is_slice = FALSE;
		break;
	}

	return VOD_OK;
}

uint8_t
avc_parser_get_transfer_characteristics(
	void* context)
{
	avc_hevc_parse_ctx_t* ctx = context;
	avc_sps_t** cur = (avc_sps_t**)ctx->sps.elts;
	avc_sps_t** end = cur + ctx->sps.nelts;
	avc_sps_t* sps;

	for (; cur < end; cur++)
	{
		sps = *cur;
		if (sps == NULL)
		{
			continue;
		}

		if (sps->transfer_characteristics != 0)
		{
			return sps->transfer_characteristics;
		}
	}

	return 0;
}
