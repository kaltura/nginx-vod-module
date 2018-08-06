#include "avc_hevc_parser.h"
#include "codec_config.h"
#include "hevc_parser.h"
#include "avc_defs.h"

// constants
#define HEVC_NAL_HEADER_SIZE (2)
#define HEVC_MAX_SHORT_TERM_RPS_COUNT (64)
#define HEVC_MAX_LONG_TERM_RPS_COUNT (32)
#define HEVC_MAX_REFS (16)

enum {
	HEVC_SLICE_B,
	HEVC_SLICE_P,
	HEVC_SLICE_I,
};

typedef struct {
	unsigned num_delta_pocs;
	int32_t delta_poc[32];
	uint8_t used[32];
} hevc_short_term_rps_t;		// matches ffmpeg's ShortTermRPS

typedef struct {
	uint32_t log2_min_luma_coding_block_size;
	uint32_t log2_diff_max_min_luma_coding_block_size;
	uint32_t pic_width_in_luma_samples;
	uint32_t pic_height_in_luma_samples;
	uint32_t log2_max_pic_order_cnt_lsb;
	uint32_t num_short_term_ref_pic_sets;
	uint32_t num_long_term_ref_pics_sps;
	uint32_t used_by_curr_pic_lt_sps_flag;
	uint32_t bit_depth_luma;
	uint32_t bit_depth_chroma;
	hevc_short_term_rps_t* st_rps;
	uint8_t transfer_characteristics;

	unsigned sps_max_sub_layers_minus1 : 3;
	unsigned chroma_format_idc : 2;
	unsigned motion_vector_resolution_control_idc : 2;
	unsigned separate_colour_plane_flag : 1;
	unsigned sample_adaptive_offset_enabled_flag : 1;
	unsigned long_term_ref_pics_present_flag : 1;
	unsigned sps_temporal_mvp_enabled_flag : 1;
} hevc_sps_t;

typedef struct {
	hevc_sps_t* sps;
	uint32_t num_ref_idx[2];

	unsigned num_extra_slice_header_bits : 3;
	unsigned slice_segment_header_extension_present_flag : 1;
	unsigned pps_loop_filter_across_slices_enabled_flag : 1;
	unsigned pps_slice_chroma_qp_offsets_present_flag : 1;
	unsigned deblocking_filter_override_enabled_flag : 1;
	unsigned pps_slice_act_qp_offsets_present_flag : 1;
	unsigned dependent_slice_segments_enabled_flag : 1;
	unsigned pps_deblocking_filter_disabled_flag : 1;
	unsigned chroma_qp_offset_list_enabled_flag : 1;
	unsigned entropy_coding_sync_enabled_flag : 1;
	unsigned lists_modification_present_flag : 1;
	unsigned pps_curr_pic_ref_enabled_flag : 1;
	unsigned output_flag_present_flag : 1;
	unsigned cabac_init_present_flag : 1;
	unsigned weighted_bipred_flag : 1;
	unsigned weighted_pred_flag : 1;
	unsigned tiles_enabled_flag : 1;
} hevc_pps_t;

// SPS
static void
hevc_parser_sub_layer_hrd_parameters(
	bit_reader_state_t* reader,
	uint32_t cpb_cnt_minus1,
	uint32_t sub_pic_hrd_params_present_flag)
{
	uint32_t i;

	for (i = 0; i <= cpb_cnt_minus1 && !reader->stream.eof_reached; i++)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// bit_rate_value_minus1[i] 
		bit_read_stream_skip_unsigned_exp(reader);		// cpb_size_value_minus1[i] 
		if (sub_pic_hrd_params_present_flag)
		{
			bit_read_stream_skip_unsigned_exp(reader);	// cpb_size_du_value_minus1[i]
			bit_read_stream_skip_unsigned_exp(reader);	// bit_rate_du_value_minus1[i]
		}
		bit_read_stream_get_one(reader);				// cbr_flag[i]
	}
}

static void
hevc_parser_hrd_parameters(
	bit_reader_state_t* reader,
	bool_t common_inf_present_flag,
	int max_num_sublayers_minus1)
{
	uint32_t sub_pic_hrd_params_present_flag = 0;
	uint32_t nal_hrd_parameters_present_flag = 0;
	uint32_t vcl_hrd_parameters_present_flag = 0;
	uint32_t fixed_pic_rate_general_flag;
	uint32_t fixed_pic_rate_within_cvs_flag;
	uint32_t low_delay_hrd_flag;
	uint32_t cpb_cnt_minus1 = 0;
	int i;

	if (common_inf_present_flag)
	{
		nal_hrd_parameters_present_flag = bit_read_stream_get_one(reader);
		vcl_hrd_parameters_present_flag = bit_read_stream_get_one(reader);
		if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag)
		{
			sub_pic_hrd_params_present_flag = bit_read_stream_get_one(reader);
			if (sub_pic_hrd_params_present_flag)
			{
				bit_read_stream_skip(reader, 8);		// tick_divisor_minus2
				bit_read_stream_skip(reader, 5);		// du_cpb_removal_delay_increment_length_minus1
				bit_read_stream_get_one(reader);		// sub_pic_cpb_params_in_pic_timing_sei_flag
				bit_read_stream_skip(reader, 5);		// dpb_output_delay_du_length_minus1
			}
			bit_read_stream_skip(reader, 4);			// bit_rate_scale
			bit_read_stream_skip(reader, 4);			// cpb_size_scale
			if (sub_pic_hrd_params_present_flag)
			{
				bit_read_stream_skip(reader, 4);		// cpb_size_du_scale
			}
			bit_read_stream_skip(reader, 5);			// initial_cpb_removal_delay_length_minus1
			bit_read_stream_skip(reader, 5);			// au_cpb_removal_delay_length_minus1
			bit_read_stream_skip(reader, 5);			// dpb_output_delay_length_minus1
		}
	}
	for (i = 0; i <= max_num_sublayers_minus1 && !reader->stream.eof_reached; i++)
	{
		fixed_pic_rate_general_flag = bit_read_stream_get_one(reader);
		if (!fixed_pic_rate_general_flag)
		{
			fixed_pic_rate_within_cvs_flag = bit_read_stream_get_one(reader);
		}
		else
		{
			fixed_pic_rate_within_cvs_flag = 1;
		}

		low_delay_hrd_flag = 0;
		if (fixed_pic_rate_within_cvs_flag)
		{
			bit_read_stream_skip_unsigned_exp(reader);	// elemental_duration_in_tc_minus1[i] 
		}
		else
		{
			low_delay_hrd_flag = bit_read_stream_get_one(reader);
		}
		if (!low_delay_hrd_flag)
		{
			cpb_cnt_minus1 = bit_read_stream_get_unsigned_exp(reader);
		}
		if (nal_hrd_parameters_present_flag)
		{
			hevc_parser_sub_layer_hrd_parameters(reader, cpb_cnt_minus1, sub_pic_hrd_params_present_flag);
		}
		if (vcl_hrd_parameters_present_flag)
		{
			hevc_parser_sub_layer_hrd_parameters(reader, cpb_cnt_minus1, sub_pic_hrd_params_present_flag);
		}
	}
}

static void
hevc_parser_skip_vui_parameters(
	bit_reader_state_t* reader,
	hevc_sps_t* sps)
{
	uint32_t chroma_loc_info_present_flag;
	uint32_t colour_description_present_flag;
	uint32_t aspect_ratio_info_present_flag;
	uint32_t video_signal_type_present_flag;
	uint32_t overscan_info_present_flag;
	uint32_t default_display_window_flag;
	uint32_t vui_timing_info_present_flag;
	uint32_t vui_poc_proportional_to_timing_flag;
	uint32_t vui_hrd_parameters_present_flag;
	uint32_t bitstream_restriction_flag;
	uint32_t aspect_ratio_idc;

	aspect_ratio_info_present_flag = bit_read_stream_get_one(reader);
	if (aspect_ratio_info_present_flag)
	{
		aspect_ratio_idc = bit_read_stream_get(reader, 8);
		if (aspect_ratio_idc == EXTENDED_SAR)
		{
			bit_read_stream_skip(reader, 16);			// sar_width
			bit_read_stream_skip(reader, 16);			// sar_height
		}
	}
	overscan_info_present_flag = bit_read_stream_get_one(reader);
	if (overscan_info_present_flag)
	{
		bit_read_stream_get_one(reader);				// overscan_appropriate_flag
	}
	video_signal_type_present_flag = bit_read_stream_get_one(reader);
	if (video_signal_type_present_flag)
	{
		bit_read_stream_skip(reader, 3);				// video_format
		bit_read_stream_get_one(reader);				// video_full_range_flag
		colour_description_present_flag = bit_read_stream_get_one(reader);
		if (colour_description_present_flag)
		{
			bit_read_stream_skip(reader, 8);			// colour_primaries
			sps->transfer_characteristics = bit_read_stream_get(reader, 8);
			bit_read_stream_skip(reader, 8);			// matrix_coeffs 
		}
	}
	chroma_loc_info_present_flag = bit_read_stream_get_one(reader);
	if (chroma_loc_info_present_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// chroma_sample_loc_type_top_field
		bit_read_stream_skip_unsigned_exp(reader);		// chroma_sample_loc_type_bottom_field
	}
	bit_read_stream_get_one(reader);					// neutral_chroma_indication_flag
	bit_read_stream_get_one(reader);					// field_seq_flag
	bit_read_stream_get_one(reader);					// frame_field_info_present_flag
	default_display_window_flag = bit_read_stream_get_one(reader);
	if (default_display_window_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// def_disp_win_left_offset 
		bit_read_stream_skip_unsigned_exp(reader);		// def_disp_win_right_offset 
		bit_read_stream_skip_unsigned_exp(reader);		// def_disp_win_top_offset 
		bit_read_stream_skip_unsigned_exp(reader);		// def_disp_win_bottom_offset 
	}
	vui_timing_info_present_flag = bit_read_stream_get_one(reader);
	if (vui_timing_info_present_flag)
	{
		bit_read_stream_skip(reader, 32);				// vui_num_units_in_tick
		bit_read_stream_skip(reader, 32);				// vui_time_scale
		vui_poc_proportional_to_timing_flag = bit_read_stream_get_one(reader);
		if (vui_poc_proportional_to_timing_flag)
		{
			bit_read_stream_skip_unsigned_exp(reader);	// vui_num_ticks_poc_diff_one_minus1
		}
		vui_hrd_parameters_present_flag = bit_read_stream_get_one(reader);
		if (vui_hrd_parameters_present_flag)
		{
			hevc_parser_hrd_parameters(reader, 1, sps->sps_max_sub_layers_minus1);
		}
	}
	bitstream_restriction_flag = bit_read_stream_get_one(reader);
	if (bitstream_restriction_flag)
	{
		bit_read_stream_get_one(reader);				// tiles_fixed_structure_flag 
		bit_read_stream_get_one(reader);				// motion_vectors_over_pic_boundaries_flag
		bit_read_stream_get_one(reader);				// restricted_ref_pic_lists_flag 
		bit_read_stream_skip_unsigned_exp(reader);		// min_spatial_segmentation_idc
		bit_read_stream_skip_unsigned_exp(reader);		// max_bytes_per_pic_denom 
		bit_read_stream_skip_unsigned_exp(reader);		// max_bits_per_min_cu_denom
		bit_read_stream_skip_unsigned_exp(reader);		// log2_max_mv_length_horizontal
		bit_read_stream_skip_unsigned_exp(reader);		// log2_max_mv_length_vertical
	}
}

static void
hevc_parser_skip_sps_range_extension(
	bit_reader_state_t* reader)
{
	bit_read_stream_get_one(reader);					// transform_skip_rotation_enabled_flag 
	bit_read_stream_get_one(reader);					// transform_skip_context_enabled_flag 
	bit_read_stream_get_one(reader);					// implicit_rdpcm_enabled_flag 
	bit_read_stream_get_one(reader);					// explicit_rdpcm_enabled_flag 
	bit_read_stream_get_one(reader);					// extended_precision_processing_flag 
	bit_read_stream_get_one(reader);					// intra_smoothing_disabled_flag 
	bit_read_stream_get_one(reader);					// high_precision_offsets_enabled_flag 
	bit_read_stream_get_one(reader);					// persistent_rice_adaptation_enabled_flag 
	bit_read_stream_get_one(reader);					// cabac_bypass_alignment_enabled_flag 
}

static void
hevc_parser_parse_sps_scc_extension(
	bit_reader_state_t* reader,
	hevc_sps_t* sps)
{
	uint32_t sps_palette_predictor_initializer_present_flag;
	uint32_t palette_mode_enabled_flag;
	int sps_num_palette_predictor_initializer_minus1;
	int num_comps;
	int comp;
	int i;

	bit_read_stream_get_one(reader);					// sps_curr_pic_ref_enabled_flag
	palette_mode_enabled_flag = bit_read_stream_get_one(reader);
	if (palette_mode_enabled_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// palette_max_size
		bit_read_stream_skip_unsigned_exp(reader);		// delta_palette_max_predictor_size
		sps_palette_predictor_initializer_present_flag = bit_read_stream_get_one(reader);
		if (sps_palette_predictor_initializer_present_flag)
		{
			sps_num_palette_predictor_initializer_minus1 = bit_read_stream_get_unsigned_exp(reader);
			num_comps = (sps->chroma_format_idc == 0) ? 1 : 3;
			for (comp = 0; comp < num_comps; comp++)
			{
				for (i = 0; i <= sps_num_palette_predictor_initializer_minus1 && !reader->stream.eof_reached; i++)
				{
					bit_read_stream_skip(reader, comp == 0 ? sps->bit_depth_luma : sps->bit_depth_chroma);	// sps_palette_predictor_initializers[comp][i] 
				}
			}
		}
	}
	sps->motion_vector_resolution_control_idc = bit_read_stream_get(reader, 2);
	bit_read_stream_get_one(reader);					// intra_boundary_filtering_disabled_flag
}

static void
hevc_parser_skip_sps_multilayer_extension(
	bit_reader_state_t* reader)
{
	bit_read_stream_get_one(reader);					// inter_view_mv_vert_constraint_flag
}

static void
hevc_parser_skip_sps_3d_extension(
	bit_reader_state_t* reader)
{
	uint32_t d;

	for (d = 0; d <= 1; d++)
	{
		bit_read_stream_get_one(reader);				// iv_di_mc_enabled_flag[d] 
		bit_read_stream_get_one(reader);				// iv_mv_scal_enabled_flag[d] 
		if (d == 0)
		{
			bit_read_stream_skip_unsigned_exp(reader);	// log2_ivmc_sub_pb_size_minus3[d] 
			bit_read_stream_get_one(reader);			// iv_res_pred_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// depth_ref_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// vsp_mc_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// dbbp_enabled_flag[d] 
		}
		else
		{
			bit_read_stream_get_one(reader);			// tex_mc_enabled_flag[d] 
			bit_read_stream_skip_unsigned_exp(reader);	// log2_texmc_sub_pb_size_minus3[d] 
			bit_read_stream_get_one(reader);			// intra_contour_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// intra_dc_only_wedge_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// cqt_cu_part_pred_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// inter_dc_only_enabled_flag[d] 
			bit_read_stream_get_one(reader);			// skip_intra_enabled_flag[d] 
		}
	}
}

static void
hevc_parser_skip_profile_tier_level(
	bit_reader_state_t* reader,
	bool_t profile_present_flag,
	uint32_t max_num_sublayers_minus1)
{
	uint32_t sub_layer_profile_present_flag = 0;
	uint32_t sub_layer_level_present_flag = 0;
	uint32_t i;

	if (profile_present_flag)
	{
		bit_read_stream_skip(reader, 2);				// general_profile_space
		bit_read_stream_get_one(reader);				// general_tier_flag 
		bit_read_stream_skip(reader, 5);				// general_profile_idc 
		bit_read_stream_skip(reader, 32);				// general_profile_compatibility_flag[j]
		bit_read_stream_get_one(reader);				// general_progressive_source_flag 
		bit_read_stream_get_one(reader);				// general_interlaced_source_flag 
		bit_read_stream_get_one(reader);				// general_non_packed_constraint_flag 
		bit_read_stream_get_one(reader);				// general_frame_only_constraint_flag 
		bit_read_stream_skip(reader, 43);				// general_reserved_zero_43bits 
		bit_read_stream_get_one(reader);				// general_reserved_zero_bit 
	}

	bit_read_stream_skip(reader, 8);					// general_level_idc 
	for (i = 0; i < max_num_sublayers_minus1 && !reader->stream.eof_reached; i++)
	{
		sub_layer_profile_present_flag |= bit_read_stream_get_one(reader) << i;
		sub_layer_level_present_flag |= bit_read_stream_get_one(reader) << i;
	}

	if (max_num_sublayers_minus1 > 0)
	{
		for (i = max_num_sublayers_minus1; i < 8; i++)
		{
			bit_read_stream_skip(reader, 2);			// reserved_zero_2bits[i] 
		}
	}

	for (i = 0; i < max_num_sublayers_minus1 && !reader->stream.eof_reached; i++)
	{
		if ((sub_layer_profile_present_flag >> i) & 1)
		{
			bit_read_stream_skip(reader, 2);			// sub_layer_profile_space[i] 
			bit_read_stream_get_one(reader);			// sub_layer_tier_flag[i] 
			bit_read_stream_skip(reader, 5);			// sub_layer_profile_idc[i] 
			bit_read_stream_skip(reader, 32);			// sub_layer_profile_compatibility_flag[i][j] 
			bit_read_stream_get_one(reader);			// sub_layer_progressive_source_flag[i] 
			bit_read_stream_get_one(reader);			// sub_layer_interlaced_source_flag[i] 
			bit_read_stream_get_one(reader);			// sub_layer_non_packed_constraint_flag[i] 
			bit_read_stream_get_one(reader);			// sub_layer_frame_only_constraint_flag[i] 
			bit_read_stream_skip(reader, 43);			// sub_layer_reserved_zero_43bits[i] 
			bit_read_stream_get_one(reader);			// sub_layer_reserved_zero_bit[i] 
		}

		if ((sub_layer_level_present_flag >> i) & 1)
		{
			bit_read_stream_skip(reader, 8);			// sub_layer_level_idc[i] 
		}
	}
}

static void
hevc_parser_skip_scaling_list_data(bit_reader_state_t* reader)
{
	uint32_t scaling_list_pred_mode_flag;
	uint32_t matrix_id_increment;
	uint32_t matrix_id;
	uint32_t coef_num;
	uint32_t size_id;
	uint32_t i;

	for (size_id = 0; size_id < 4; size_id++)
	{
		matrix_id_increment = (size_id == 3) ? 3 : 1;
		for (matrix_id = 0; matrix_id < 6; matrix_id += matrix_id_increment)
		{
			scaling_list_pred_mode_flag = bit_read_stream_get_one(reader);
			if (!scaling_list_pred_mode_flag)
			{
				bit_read_stream_skip_unsigned_exp(reader);		// scaling_list_pred_matrix_id_delta[size_id][matrix_id] 
			}
			else
			{
				coef_num = vod_min(64, (1 << (4 + (size_id << 1))));
				if (size_id > 1)
				{
					bit_read_stream_skip_signed_exp(reader);	// scaling_list_dc_coef_minus8[size_id − 2][matrix_id] 
				}
				for (i = 0; i < coef_num && !reader->stream.eof_reached; i++)
				{
					bit_read_stream_skip_signed_exp(reader);	// scaling_list_delta_coef
				}
			}
		}
	}
}

// based on ff_hevc_decode_short_term_rps
static vod_status_t
hevc_parser_skip_st_ref_pic_set(
	avc_hevc_parse_ctx_t* ctx,
	bit_reader_state_t* reader,
	hevc_sps_t* sps,
	uint32_t st_rps_idx,
	hevc_short_term_rps_t* rps)
{
	hevc_short_term_rps_t* rps_ridx;
	uint32_t inter_ref_pic_set_prediction_flag = 0;
	uint32_t num_positive_pics;
	uint32_t num_negative_pics;
	uint32_t use_delta_flag;
	uint32_t delta_idx = 1;
	uint32_t used;
	uint32_t i;
	uint8_t delta_rps_sign;
	unsigned abs_delta_rps;
	unsigned prev;
	int delta_poc;
	int delta_rps;
	int tmp;
	int k;

	if (st_rps_idx != 0)
	{
		inter_ref_pic_set_prediction_flag = bit_read_stream_get_one(reader);
	}

	if (inter_ref_pic_set_prediction_flag)
	{
		if (st_rps_idx == sps->num_short_term_ref_pic_sets)
		{
			delta_idx = bit_read_stream_get_unsigned_exp(reader) + 1;
			if (delta_idx > st_rps_idx)
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"hevc_parser_skip_st_ref_pic_set: invalid delta_idx_minus1");
				return VOD_BAD_DATA;
			}
		}
		
		rps_ridx = &sps->st_rps[st_rps_idx - delta_idx];

		delta_rps_sign = bit_read_stream_get_one(reader);
		abs_delta_rps = bit_read_stream_get_unsigned_exp(reader) + 1;
		if (abs_delta_rps < 1 || abs_delta_rps > 32768) 
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_skip_st_ref_pic_set: invalid abs_delta_rps %u", abs_delta_rps);
			return VOD_BAD_DATA;
		}

		delta_rps = (1 - (delta_rps_sign << 1)) * abs_delta_rps;

		num_negative_pics = 0;
		k = 0;

		for (i = 0; i <= rps_ridx->num_delta_pocs && !reader->stream.eof_reached; i++)
		{
			int used = rps->used[k] = bit_read_stream_get_one(reader);
			
			if (!used)
			{
				use_delta_flag = bit_read_stream_get_one(reader);
			}
			else
			{
				use_delta_flag = 1;
			}

			if (used || use_delta_flag)
			{
				if (i < rps_ridx->num_delta_pocs)
				{
					delta_poc = delta_rps + rps_ridx->delta_poc[i];
				}
				else
				{
					delta_poc = delta_rps;
				}

				rps->delta_poc[k] = delta_poc;
				if (delta_poc < 0)
				{
					num_negative_pics++;
				}
				k++;
			}
		}

		rps->num_delta_pocs = k;

		// sort in increasing order (smallest first)
		for (i = 1; i < rps->num_delta_pocs; i++)
		{
			delta_poc = rps->delta_poc[i];
			used = rps->used[i];
			for (k = i - 1; k >= 0; k--) 
			{
				tmp = rps->delta_poc[k];
				if (delta_poc < tmp) 
				{
					rps->delta_poc[k + 1] = tmp;
					rps->used[k + 1] = rps->used[k];
					rps->delta_poc[k] = delta_poc;
					rps->used[k] = used;
				}
			}
		}

		// flip the negative values to largest first
		for (i = 0, k = num_negative_pics - 1;
			i < num_negative_pics >> 1; 
			i++, k--)
		{
			delta_poc = rps->delta_poc[i];
			used = rps->used[i];
			rps->delta_poc[i] = rps->delta_poc[k];
			rps->used[i] = rps->used[k];
			rps->delta_poc[k] = delta_poc;
			rps->used[k] = used;
		}
	}
	else
	{
		num_negative_pics = bit_read_stream_get_unsigned_exp(reader);
		num_positive_pics = bit_read_stream_get_unsigned_exp(reader);

		if (num_negative_pics >= HEVC_MAX_REFS ||
			num_positive_pics >= HEVC_MAX_REFS) 
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_skip_st_ref_pic_set: too many refs");
			return VOD_BAD_DATA;
		}

		rps->num_delta_pocs = num_negative_pics + num_positive_pics;

		prev = 0;
		for (i = 0; i < num_negative_pics && !reader->stream.eof_reached; i++)
		{
			delta_poc = bit_read_stream_get_unsigned_exp(reader) + 1;	// delta_poc_s0_minus1[i] 
			prev -= delta_poc;
			rps->delta_poc[i] = prev;
			rps->used[i] = bit_read_stream_get_one(reader);			// used_by_curr_pic_s0_flag[i] 
		}

		prev = 0;
		for (; i < rps->num_delta_pocs && !reader->stream.eof_reached; i++)
		{
			delta_poc = bit_read_stream_get_unsigned_exp(reader) + 1;	// delta_poc_s1_minus1[i] 
			prev += delta_poc;
			rps->delta_poc[i] = prev;
			rps->used[i] = bit_read_stream_get_one(reader);			// used_by_curr_pic_s1_flag[i] 
		}
	}

	return VOD_OK;
}

static vod_status_t
hevc_parser_seq_parameter_set_rbsp(
	avc_hevc_parse_ctx_t* ctx,
	bit_reader_state_t* reader)
{
	hevc_sps_t* sps;
	uint32_t sps_sub_layer_ordering_info_present_flag;
	uint32_t sps_range_extension_flag = 0;
	uint32_t sps_multilayer_extension_flag = 0;
	uint32_t sps_3d_extension_flag = 0;
	uint32_t sps_scc_extension_flag = 0;
	uint32_t sps_extension_4bits = 0;
	uint32_t sps_extension_present_flag;
	uint32_t vui_parameters_present_flag;
	uint32_t scaling_list_enabled_flag;
	uint32_t conformance_window_flag;
	uint32_t sps_scaling_list_data_present_flag;
	uint32_t pcm_enabled_flag;
	uint32_t sps_seq_parameter_set_id;
	uint32_t sps_max_sub_layers_minus1;
	uint32_t i;
	vod_status_t rc;

	bit_read_stream_skip(reader, 4);					// sps_video_parameter_set_id
	sps_max_sub_layers_minus1 = bit_read_stream_get(reader, 3);
	bit_read_stream_get_one(reader);					// sps_temporal_id_nesting_flag
	hevc_parser_skip_profile_tier_level(reader, 1, sps_max_sub_layers_minus1);

	sps_seq_parameter_set_id = bit_read_stream_get_unsigned_exp(reader);
	if (sps_seq_parameter_set_id >= MAX_SPS_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_seq_parameter_set_rbsp: invalid sps id %uD", sps_seq_parameter_set_id);
		return VOD_BAD_DATA;
	}

	sps = avc_hevc_parser_get_ptr_array_item(&ctx->sps, sps_seq_parameter_set_id, sizeof(*sps));
	if (sps == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->request_context->log, 0,
			"hevc_parser_seq_parameter_set_rbsp: avc_hevc_parser_get_ptr_array_item failed");
		return VOD_ALLOC_FAILED;
	}

	sps->sps_max_sub_layers_minus1 = sps_max_sub_layers_minus1;
	sps->chroma_format_idc = bit_read_stream_get_unsigned_exp(reader);
	if (sps->chroma_format_idc == 3)
	{
		sps->separate_colour_plane_flag = bit_read_stream_get_one(reader);
	}
	sps->pic_width_in_luma_samples = bit_read_stream_get_unsigned_exp(reader);
	sps->pic_height_in_luma_samples = bit_read_stream_get_unsigned_exp(reader);
	conformance_window_flag = bit_read_stream_get_one(reader);
	if (conformance_window_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// conf_win_left_offset 
		bit_read_stream_skip_unsigned_exp(reader);		// conf_win_right_offset 
		bit_read_stream_skip_unsigned_exp(reader);		// conf_win_top_offset 
		bit_read_stream_skip_unsigned_exp(reader);		// conf_win_bottom_offset 
	}
	sps->bit_depth_luma = bit_read_stream_get_unsigned_exp(reader) + 8;
	sps->bit_depth_chroma = bit_read_stream_get_unsigned_exp(reader) + 8;
	sps->log2_max_pic_order_cnt_lsb = bit_read_stream_get_unsigned_exp(reader) + 4;
	sps_sub_layer_ordering_info_present_flag = bit_read_stream_get_one(reader);
	for (i = (sps_sub_layer_ordering_info_present_flag ? 0 : sps->sps_max_sub_layers_minus1);
		i <= sps->sps_max_sub_layers_minus1 && !reader->stream.eof_reached;
		i++)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// sps_max_dec_pic_buffering_minus1[i] 
		bit_read_stream_skip_unsigned_exp(reader);		// sps_max_num_reorder_pics[i] 
		bit_read_stream_skip_unsigned_exp(reader);		// sps_max_latency_increase_plus1[i] 
	}
	sps->log2_min_luma_coding_block_size = bit_read_stream_get_unsigned_exp(reader) + 3;
	sps->log2_diff_max_min_luma_coding_block_size = bit_read_stream_get_unsigned_exp(reader);
	bit_read_stream_skip_unsigned_exp(reader);			// log2_min_luma_transform_block_size_minus2 
	bit_read_stream_skip_unsigned_exp(reader);			// log2_diff_max_min_luma_transform_block_size 
	bit_read_stream_skip_unsigned_exp(reader);			// max_transform_hierarchy_depth_inter 
	bit_read_stream_skip_unsigned_exp(reader);			// max_transform_hierarchy_depth_intra 
	scaling_list_enabled_flag = bit_read_stream_get_one(reader);
	if (scaling_list_enabled_flag)
	{
		sps_scaling_list_data_present_flag = bit_read_stream_get_one(reader);
		if (sps_scaling_list_data_present_flag)
		{
			hevc_parser_skip_scaling_list_data(reader);
		}
	}
	bit_read_stream_get_one(reader);					// amp_enabled_flag 
	sps->sample_adaptive_offset_enabled_flag = bit_read_stream_get_one(reader);
	pcm_enabled_flag = bit_read_stream_get_one(reader);
	if (pcm_enabled_flag)
	{
		bit_read_stream_skip(reader, 4);				// pcm_sample_bit_depth_luma_minus1 
		bit_read_stream_skip(reader, 4);				// pcm_sample_bit_depth_chroma_minus1 
		bit_read_stream_skip_unsigned_exp(reader);		// log2_min_pcm_luma_coding_block_size_minus3 
		bit_read_stream_skip_unsigned_exp(reader);		// log2_diff_max_min_pcm_luma_coding_block_size 
		bit_read_stream_get_one(reader);				// pcm_loop_filter_disabled_flag 
	}
	sps->num_short_term_ref_pic_sets = bit_read_stream_get_unsigned_exp(reader);
	if (sps->num_short_term_ref_pic_sets > HEVC_MAX_SHORT_TERM_RPS_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_seq_parameter_set_rbsp: invalid num_short_term_ref_pic_sets %uD",
			sps->num_short_term_ref_pic_sets);
		return VOD_BAD_DATA;
	}

	sps->st_rps = vod_alloc(ctx->request_context->pool, sps->num_short_term_ref_pic_sets * sizeof(sps->st_rps[0]));
	if (sps->st_rps == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->request_context->log, 0,
			"hevc_parser_seq_parameter_set_rbsp: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	for (i = 0; i < sps->num_short_term_ref_pic_sets && !reader->stream.eof_reached; i++)
	{
		rc = hevc_parser_skip_st_ref_pic_set(ctx, reader, sps, i, sps->st_rps + i);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	sps->long_term_ref_pics_present_flag = bit_read_stream_get_one(reader);
	if (sps->long_term_ref_pics_present_flag)
	{
		sps->num_long_term_ref_pics_sps = bit_read_stream_get_unsigned_exp(reader);
		if (sps->num_long_term_ref_pics_sps > HEVC_MAX_LONG_TERM_RPS_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_seq_parameter_set_rbsp: invalid sps->num_long_term_ref_pics_sps %uD",
				sps->num_short_term_ref_pic_sets);
			return VOD_BAD_DATA;
		}

		for (i = 0; i < sps->num_long_term_ref_pics_sps && !reader->stream.eof_reached; i++)
		{
			bit_read_stream_skip(reader, sps->log2_max_pic_order_cnt_lsb);	// lt_ref_pic_poc_lsb_sps[i]
			sps->used_by_curr_pic_lt_sps_flag |= (bit_read_stream_get_one(reader) << i);			// used_by_curr_pic_lt_sps_flag[i] 
		}
	}
	sps->sps_temporal_mvp_enabled_flag = bit_read_stream_get_one(reader);
	bit_read_stream_get_one(reader);					// strong_intra_smoothing_enabled_flag
	vui_parameters_present_flag = bit_read_stream_get_one(reader);
	if (vui_parameters_present_flag)
	{
		hevc_parser_skip_vui_parameters(reader, sps);
	}
	sps_extension_present_flag = bit_read_stream_get_one(reader);
	if (sps_extension_present_flag)
	{
		sps_range_extension_flag = bit_read_stream_get_one(reader);
		sps_multilayer_extension_flag = bit_read_stream_get_one(reader);
		sps_3d_extension_flag = bit_read_stream_get_one(reader);
		sps_scc_extension_flag = bit_read_stream_get_one(reader);
		sps_extension_4bits = bit_read_stream_get(reader, 4);
	}
	if (sps_range_extension_flag)
	{
		hevc_parser_skip_sps_range_extension(reader);
	}
	if (sps_multilayer_extension_flag)
	{
		hevc_parser_skip_sps_multilayer_extension(reader);  // Annex F
	}
	if (sps_3d_extension_flag)
	{
		hevc_parser_skip_sps_3d_extension(reader);			// Annex I
	}
	if (sps_scc_extension_flag)
	{
		hevc_parser_parse_sps_scc_extension(reader, sps);
	}
	if (sps_extension_4bits)
	{
		if (reader->stream.eof_reached)
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_seq_parameter_set_rbsp: stream overflow");
			return VOD_BAD_DATA;
		}

		return VOD_OK;
	}

	if (!avc_hevc_parser_rbsp_trailing_bits(reader))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_seq_parameter_set_rbsp: invalid trailing bits");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

// PPS
static void
hevc_parser_skip_pps_range_extension(
	bit_reader_state_t* reader,
	hevc_pps_t* pps,
	uint32_t transform_skip_enabled_flag)
{
	uint32_t chroma_qp_offset_list_len_minus1;
	uint32_t i;

	if (transform_skip_enabled_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// log2_max_transform_skip_block_size_minus2
	}
	bit_read_stream_get_one(reader);					// cross_component_prediction_enabled_flag
	pps->chroma_qp_offset_list_enabled_flag = bit_read_stream_get_one(reader);
	if (pps->chroma_qp_offset_list_enabled_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// diff_cu_chroma_qp_offset_depth
		chroma_qp_offset_list_len_minus1 = bit_read_stream_get_unsigned_exp(reader);
		for (i = 0; i <= chroma_qp_offset_list_len_minus1 && !reader->stream.eof_reached; i++)
		{
			bit_read_stream_skip_signed_exp(reader);	// cb_qp_offset_list[i]
			bit_read_stream_skip_signed_exp(reader);	// cr_qp_offset_list[i] 
		}
	}
	bit_read_stream_skip_unsigned_exp(reader);			// log2_sao_offset_scale_luma
	bit_read_stream_skip_unsigned_exp(reader);			// log2_sao_offset_scale_chroma
}

static void
hevc_parser_skip_delta_dlt(
	bit_reader_state_t* reader,
	uint32_t pps_bit_depth_for_depth_layers)
{
	int num_val_delta_dlt;
	int max_diff = 0;
	int min_diff;
	int k;

	num_val_delta_dlt = bit_read_stream_get(reader, pps_bit_depth_for_depth_layers);
	if (num_val_delta_dlt > 0)
	{
		if (num_val_delta_dlt > 1)
		{
			max_diff = bit_read_stream_get(reader, pps_bit_depth_for_depth_layers);
		}
		if (num_val_delta_dlt > 2 && max_diff > 0)
		{
			min_diff = bit_read_stream_get(reader, avc_hevc_parser_ceil_log2(max_diff + 1)) + 1;
		}
		else
		{
			min_diff = max_diff;
		}
		bit_read_stream_skip(reader, pps_bit_depth_for_depth_layers);	// delta_dlt_val0
		if (max_diff > min_diff)
		{
			for (k = 1; k < num_val_delta_dlt && !reader->stream.eof_reached; k++)
			{
				bit_read_stream_skip(reader, avc_hevc_parser_ceil_log2(max_diff - min_diff + 1));		// delta_val_diff_minus_min[k]
			}
		}
	}
}

static void
hevc_parser_skip_pps_3d_extension(
	bit_reader_state_t* reader)
{
	uint32_t pps_bit_depth_for_depth_layers;
	uint32_t dlt_val_flags_present_flag;
	uint32_t dlts_present_flag;
	uint32_t dlt_pred_flag;
	uint32_t dlt_flag;
	uint32_t pps_depth_layers_minus1;
	int depth_max_value;
	int j;
	uint32_t i;

	dlts_present_flag = bit_read_stream_get_one(reader);
	if (dlts_present_flag)
	{
		pps_depth_layers_minus1 = bit_read_stream_get(reader, 6);
		pps_bit_depth_for_depth_layers = bit_read_stream_get(reader, 4) + 8;
		for (i = 0; i <= pps_depth_layers_minus1 && !reader->stream.eof_reached; i++)
		{
			dlt_flag = bit_read_stream_get_one(reader);
			if (dlt_flag)
			{
				dlt_pred_flag = bit_read_stream_get_one(reader);
				if (!dlt_pred_flag)
				{
					dlt_val_flags_present_flag = bit_read_stream_get_one(reader);
				}
				else
				{
					dlt_val_flags_present_flag = 0;
				}

				if (dlt_val_flags_present_flag)
				{
					depth_max_value = (1 << pps_bit_depth_for_depth_layers) - 1;
					for (j = 0; j <= depth_max_value && !reader->stream.eof_reached; j++)
					{
						bit_read_stream_get_one(reader);	// dlt_value_flag[i][j] 
					}
				}
				else
				{
					hevc_parser_skip_delta_dlt(reader, pps_bit_depth_for_depth_layers);
				}
			}
		}
	}
}

static void
hevc_parser_skip_colour_mapping_octants(
	bit_reader_state_t* reader,
	uint32_t cm_octant_depth,
	uint32_t part_num_y,
	uint32_t cm_res_ls_bits,
	uint32_t inp_depth,
	uint32_t idx_y,
	uint32_t idx_cb,
	uint32_t idx_cr,
	uint32_t inp_length)
{
	uint32_t split_octant_flag;
	uint32_t coded_res_flag;
	unsigned k, m, n;
	unsigned i, j, c;
	int res_coeff_r;
	int res_coeff_q;

	if (inp_depth < cm_octant_depth)
	{
		split_octant_flag = bit_read_stream_get_one(reader);
	}
	else
	{
		split_octant_flag = 0;
	}

	if (split_octant_flag)
	{
		for (k = 0; k < 2; k++)
		{
			for (m = 0; m < 2; m++)
			{
				for (n = 0; n < 2; n++)
				{
					hevc_parser_skip_colour_mapping_octants(
						reader,
						cm_octant_depth,
						part_num_y,
						cm_res_ls_bits,
						inp_depth + 1,
						idx_y + part_num_y * k * inp_length / 2,
						idx_cb + m * inp_length / 2,
						idx_cr + n * inp_length / 2,
						inp_length / 2);
				}
			}
		}
	}
	else
	{
		for (i = 0; i < part_num_y && !reader->stream.eof_reached; i++)
		{
			//idxShiftY = idx_y + ((i << (cm_octant_depth − inp_depth));
			for (j = 0; j < 4; j++)
			{
				coded_res_flag = bit_read_stream_get_one(reader);	// [ idxShiftY ][ idx_cb ][ idx_cr ][ j ]
				if (coded_res_flag)			// [ idxShiftY ][ idx_cb ][ idx_cr ][ j ]
				{
					for (c = 0; c < 3; c++)
					{
						res_coeff_q = bit_read_stream_get_unsigned_exp(reader);       // [idxShiftY][idx_cb][idx_cr][j][c]
						res_coeff_r = bit_read_stream_get(reader, cm_res_ls_bits);	// [idxShiftY][idx_cb][idx_cr][j][c]
						if (res_coeff_q || res_coeff_r)
						{
							bit_read_stream_get_one(reader);	// res_coeff_s[idxShiftY][idx_cb][idx_cr][j][c] 
						}
					}
				}
			}
		}
	}
}

static void
hevc_parser_skip_colour_mapping_table(
	bit_reader_state_t* reader)
{
	uint32_t cm_y_part_num_log2;
	uint32_t cm_octant_depth;
	uint32_t cm_res_quant_bits;
	uint32_t cm_delta_flc_bits;
	uint32_t luma_bit_depth_cm_input;
	uint32_t luma_bit_depth_cm_output;
	uint32_t num_cm_ref_layers_minus1;
	uint32_t cm_res_ls_bits;
	uint32_t temp;
	uint32_t i;

	num_cm_ref_layers_minus1 = bit_read_stream_get_unsigned_exp(reader);
	for (i = 0; i <= num_cm_ref_layers_minus1 && !reader->stream.eof_reached; i++)
	{
		bit_read_stream_skip(reader, 6);				// cm_ref_layer_id[i] 
	}
	cm_octant_depth = bit_read_stream_get(reader, 2);
	cm_y_part_num_log2 = bit_read_stream_get(reader, 2);
	luma_bit_depth_cm_input = bit_read_stream_get_unsigned_exp(reader) + 8;
	bit_read_stream_skip_unsigned_exp(reader);			// chroma_bit_depth_cm_input_minus8
	luma_bit_depth_cm_output = bit_read_stream_get_unsigned_exp(reader) + 8;
	bit_read_stream_skip_unsigned_exp(reader);			// chroma_bit_depth_cm_output_minus8
	cm_res_quant_bits = bit_read_stream_get(reader, 2);
	cm_delta_flc_bits = bit_read_stream_get(reader, 2);

	temp = luma_bit_depth_cm_output + cm_res_quant_bits + cm_delta_flc_bits;
	cm_res_ls_bits = 10 + luma_bit_depth_cm_input;
	if (cm_res_ls_bits > temp)
	{
		cm_res_ls_bits -= temp;
	}
	else
	{
		cm_res_ls_bits = 0;
	}

	if (cm_octant_depth == 1)
	{
		bit_read_stream_skip_signed_exp(reader);		// cm_adapt_threshold_u_delta
		bit_read_stream_skip_signed_exp(reader);		// cm_adapt_threshold_v_delta
	}
	hevc_parser_skip_colour_mapping_octants(
		reader, 
		cm_octant_depth, 
		1 << cm_y_part_num_log2, 
		cm_res_ls_bits, 
		0, 0, 0, 0, 
		1 << cm_octant_depth);
}

static void
hevc_parser_skip_pps_multilayer_extension(
	bit_reader_state_t* reader)
{
	uint32_t pps_infer_scaling_list_flag;
	uint32_t scaled_ref_layer_offset_present_flag;
	uint32_t ref_region_offset_present_flag;
	uint32_t resample_phase_set_present_flag;
	uint32_t colour_mapping_enabled_flag;
	int num_ref_loc_offsets;
	int i;

	bit_read_stream_get_one(reader);					// poc_reset_info_present_flag
	pps_infer_scaling_list_flag = bit_read_stream_get_one(reader);
	if (pps_infer_scaling_list_flag)
	{
		bit_read_stream_skip(reader, 6);				// pps_scaling_list_ref_layer_id
	}
	num_ref_loc_offsets = bit_read_stream_get_unsigned_exp(reader);
	for (i = 0; i < num_ref_loc_offsets && !reader->stream.eof_reached; i++)
	{
		bit_read_stream_skip(reader, 6);				//ref_loc_offset_layer_id[i]
		scaled_ref_layer_offset_present_flag = bit_read_stream_get_one(reader);
		if (scaled_ref_layer_offset_present_flag)
		{
			bit_read_stream_skip_signed_exp(reader);	// scaled_ref_layer_left_offset[ref_loc_offset_layer_id[i]]
			bit_read_stream_skip_signed_exp(reader);	// scaled_ref_layer_top_offset[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_signed_exp(reader);	// scaled_ref_layer_right_offset[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_signed_exp(reader);	// scaled_ref_layer_bottom_offset[ref_loc_offset_layer_id[i]] 
		}
		ref_region_offset_present_flag = bit_read_stream_get_one(reader);
		if (ref_region_offset_present_flag)
		{
			bit_read_stream_skip_signed_exp(reader);	// ref_region_left_offset[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_signed_exp(reader);	// ref_region_top_offset[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_signed_exp(reader);	// ref_region_right_offset[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_signed_exp(reader);	// ref_region_bottom_offset[ref_loc_offset_layer_id[i]] 
		}
		resample_phase_set_present_flag = bit_read_stream_get_one(reader);
		if (resample_phase_set_present_flag)
		{
			bit_read_stream_skip_unsigned_exp(reader);	// phase_hor_luma[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_unsigned_exp(reader);	// phase_ver_luma[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_unsigned_exp(reader);	// phase_hor_chroma_plus8[ref_loc_offset_layer_id[i]] 
			bit_read_stream_skip_unsigned_exp(reader);	// phase_ver_chroma_plus8[ref_loc_offset_layer_id[i]] 
		}
	}
	colour_mapping_enabled_flag = bit_read_stream_get_one(reader);
	if (colour_mapping_enabled_flag)
	{
		hevc_parser_skip_colour_mapping_table(reader);
	}
}

static void
hevc_parser_skip_pps_scc_extension(
	bit_reader_state_t* reader,
	hevc_pps_t* pps)
{
	uint32_t residual_adaptive_colour_transform_enabled_flag;
	uint32_t pps_palette_predictor_initializer_present_flag;
	uint32_t monochrome_palette_flag;
	int pps_num_palette_predictor_initializer;
	int luma_bit_depth_entry;
	int chroma_bit_depth_entry = 0;
	int num_comps;
	int comp;
	int i;

	pps->pps_curr_pic_ref_enabled_flag = bit_read_stream_get_one(reader);
	residual_adaptive_colour_transform_enabled_flag = bit_read_stream_get_one(reader);
	if (residual_adaptive_colour_transform_enabled_flag)
	{
		pps->pps_slice_act_qp_offsets_present_flag = bit_read_stream_get_one(reader);
		bit_read_stream_skip_signed_exp(reader);		// pps_act_y_qp_offset_plus5 
		bit_read_stream_skip_signed_exp(reader);		// pps_act_cb_qp_offset_plus5 
		bit_read_stream_skip_signed_exp(reader);		// pps_act_cr_qp_offset_plus3 
	}
	pps_palette_predictor_initializer_present_flag = bit_read_stream_get_one(reader);
	if (pps_palette_predictor_initializer_present_flag)
	{
		pps_num_palette_predictor_initializer = bit_read_stream_get_unsigned_exp(reader);
		if (pps_num_palette_predictor_initializer > 0)
		{
			monochrome_palette_flag = bit_read_stream_get_one(reader);
			luma_bit_depth_entry = bit_read_stream_get_unsigned_exp(reader) + 8;
			if (!monochrome_palette_flag)
			{
				chroma_bit_depth_entry = bit_read_stream_get_unsigned_exp(reader) + 8;
			}
			num_comps = monochrome_palette_flag ? 1 : 3;
			for (comp = 0; comp < num_comps; comp++)
			{
				for (i = 0; i < pps_num_palette_predictor_initializer && !reader->stream.eof_reached; i++)
				{
					bit_read_stream_skip(reader,		// pps_palette_predictor_initializers[comp][i]
						comp == 0 ? luma_bit_depth_entry : chroma_bit_depth_entry);
				}
			}
		}
	}
}

static vod_status_t
hevc_parser_pic_parameter_set_rbsp(
	avc_hevc_parse_ctx_t* ctx,
	bit_reader_state_t* reader)
{
	hevc_pps_t* pps;
	uint32_t transform_skip_enabled_flag;
	uint32_t cu_qp_delta_enabled_flag;
	uint32_t uniform_spacing_flag;
	uint32_t deblocking_filter_control_present_flag;
	uint32_t pps_range_extension_flag = 0;
	uint32_t pps_multilayer_extension_flag = 0;
	uint32_t pps_3d_extension_flag = 0;
	uint32_t pps_scc_extension_flag = 0;
	uint32_t pps_extension_4bits = 0;
	uint32_t pps_scaling_list_data_present_flag;
	uint32_t pps_extension_present_flag;
	uint32_t pps_pic_parameter_set_id;
	uint32_t pps_seq_parameter_set_id;
	int num_tile_columns_minus1;
	int num_tile_rows_minus1;
	int i;

	pps_pic_parameter_set_id = bit_read_stream_get_unsigned_exp(reader);		// pps_pic_parameter_set_id

	if (pps_pic_parameter_set_id >= MAX_PPS_COUNT)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_pic_parameter_set_rbsp: invalid pps id %uD", pps_pic_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps = avc_hevc_parser_get_ptr_array_item(&ctx->pps, pps_pic_parameter_set_id, sizeof(*pps));
	if (pps == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, ctx->request_context->log, 0,
			"hevc_parser_pic_parameter_set_rbsp: avc_hevc_parser_get_ptr_array_item failed");
		return VOD_ALLOC_FAILED;
	}

	pps_seq_parameter_set_id = bit_read_stream_get_unsigned_exp(reader);		// pps_seq_parameter_set_id
	if (pps_seq_parameter_set_id >= ctx->sps.nelts)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_pic_parameter_set_rbsp: invalid sps id %uD", pps_seq_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps->sps = ((hevc_sps_t**)ctx->sps.elts)[pps_seq_parameter_set_id];
	if (pps->sps == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_pic_parameter_set_rbsp: non-existing sps id %uD", pps_seq_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps->dependent_slice_segments_enabled_flag = bit_read_stream_get_one(reader);
	pps->output_flag_present_flag = bit_read_stream_get_one(reader);
	pps->num_extra_slice_header_bits = bit_read_stream_get(reader, 3);
	bit_read_stream_get_one(reader);					// sign_data_hiding_enabled_flag
	pps->cabac_init_present_flag = bit_read_stream_get_one(reader);
	pps->num_ref_idx[0] = bit_read_stream_get_unsigned_exp(reader) + 1;
	pps->num_ref_idx[1] = bit_read_stream_get_unsigned_exp(reader) + 1;
	bit_read_stream_skip_signed_exp(reader);			// init_qp_minus26
	bit_read_stream_get_one(reader);					// constrained_intra_pred_flag
	transform_skip_enabled_flag = bit_read_stream_get_one(reader);
	cu_qp_delta_enabled_flag = bit_read_stream_get_one(reader);
	if (cu_qp_delta_enabled_flag)
	{
		bit_read_stream_skip_unsigned_exp(reader);		// diff_cu_qp_delta_depth
	}
	bit_read_stream_skip_signed_exp(reader);			// pps_cb_qp_offset
	bit_read_stream_skip_signed_exp(reader);			// pps_cr_qp_offset
	pps->pps_slice_chroma_qp_offsets_present_flag = bit_read_stream_get_one(reader);
	pps->weighted_pred_flag = bit_read_stream_get_one(reader);
	pps->weighted_bipred_flag = bit_read_stream_get_one(reader);
	bit_read_stream_get_one(reader);					// transquant_bypass_enabled_flag
	pps->tiles_enabled_flag = bit_read_stream_get_one(reader);
	pps->entropy_coding_sync_enabled_flag = bit_read_stream_get_one(reader);
	if (pps->tiles_enabled_flag)
	{
		num_tile_columns_minus1 = bit_read_stream_get_unsigned_exp(reader);
		num_tile_rows_minus1 = bit_read_stream_get_unsigned_exp(reader);
		uniform_spacing_flag = bit_read_stream_get_one(reader);
		if (!uniform_spacing_flag)
		{
			for (i = 0; i < num_tile_columns_minus1 && !reader->stream.eof_reached; i++)
			{
				bit_read_stream_skip_unsigned_exp(reader);		// column_width_minus1[i]
			}
			for (i = 0; i < num_tile_rows_minus1 && !reader->stream.eof_reached; i++)
			{
				bit_read_stream_skip_unsigned_exp(reader);		// row_height_minus1[i]
			}
		}
		bit_read_stream_get_one(reader);				// loop_filter_across_tiles_enabled_flag
	}
	pps->pps_loop_filter_across_slices_enabled_flag = bit_read_stream_get_one(reader);
	deblocking_filter_control_present_flag = bit_read_stream_get_one(reader);
	if (deblocking_filter_control_present_flag)
	{
		pps->deblocking_filter_override_enabled_flag = bit_read_stream_get_one(reader);
		pps->pps_deblocking_filter_disabled_flag = bit_read_stream_get_one(reader);
		if (!pps->pps_deblocking_filter_disabled_flag)
		{
			bit_read_stream_skip_signed_exp(reader);	// pps_beta_offset_div2
			bit_read_stream_skip_signed_exp(reader);	// pps_tc_offset_div2
		}
	}
	pps_scaling_list_data_present_flag = bit_read_stream_get_one(reader);
	if (pps_scaling_list_data_present_flag)
	{
		hevc_parser_skip_scaling_list_data(reader);
	}
	pps->lists_modification_present_flag = bit_read_stream_get_one(reader);
	bit_read_stream_skip_unsigned_exp(reader);			// log2_parallel_merge_level_minus2
	pps->slice_segment_header_extension_present_flag = bit_read_stream_get_one(reader);
	pps_extension_present_flag = bit_read_stream_get_one(reader);
	if (pps_extension_present_flag)
	{
		pps_range_extension_flag = bit_read_stream_get_one(reader);
		pps_multilayer_extension_flag = bit_read_stream_get_one(reader);
		pps_3d_extension_flag = bit_read_stream_get_one(reader);
		pps_scc_extension_flag = bit_read_stream_get_one(reader);
		pps_extension_4bits = bit_read_stream_get(reader, 4);
	}
	if (pps_range_extension_flag)
	{
		hevc_parser_skip_pps_range_extension(reader, pps, transform_skip_enabled_flag);
	}
	if (pps_multilayer_extension_flag)
	{
		hevc_parser_skip_pps_multilayer_extension(reader);
	}
	if (pps_3d_extension_flag)
	{
		hevc_parser_skip_pps_3d_extension(reader);
	}
	if (pps_scc_extension_flag)
	{
		hevc_parser_skip_pps_scc_extension(reader, pps);
	}
	if (pps_extension_4bits)
	{
		if (reader->stream.eof_reached)
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_pic_parameter_set_rbsp: stream overflow");
			return VOD_BAD_DATA;
		}

		return VOD_OK;
	}

	if (!avc_hevc_parser_rbsp_trailing_bits(reader))
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_pic_parameter_set_rbsp: invalid trailing bits");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

// extra data
vod_status_t
hevc_parser_parse_extra_data(
	void* context,
	vod_str_t* extra_data,
	uint32_t* nal_packet_size_length,
	uint32_t* min_packet_size)
{
	avc_hevc_parse_ctx_t* ctx = context;
	bit_reader_state_t reader;
	hevc_config_t cfg;
	vod_status_t rc;
	const u_char* start_pos;
	const u_char* cur_pos;
	const u_char* end_pos;
	uint16_t unit_size;
	uint16_t count;
	uint8_t type_count;
	uint8_t nal_type;

	rc = codec_config_hevc_config_parse(ctx->request_context, extra_data, &cfg, &start_pos);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (nal_packet_size_length != NULL)
	{
		*nal_packet_size_length = cfg.nal_unit_size;
	}

	if (min_packet_size != NULL)
	{
		*min_packet_size = *nal_packet_size_length + HEVC_NAL_HEADER_SIZE;
	}

	end_pos = extra_data->data + extra_data->len;

	// calculate total size and validate
	cur_pos = start_pos;
	if (cur_pos >= end_pos)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_parse_extra_data: extra data overflow while reading type count");
		return VOD_BAD_DATA;
	}

	for (type_count = *cur_pos++; type_count > 0; type_count--)
	{
		if (end_pos - cur_pos < 3)
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_parse_extra_data: extra data overflow while reading type header");
			return VOD_BAD_DATA;
		}

		cur_pos++;
		read_be16(cur_pos, count);

		for (; count > 0; count--)
		{
			if ((size_t)(end_pos - cur_pos) < sizeof(uint16_t))
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"hevc_parser_parse_extra_data: extra data overflow while reading unit size");
				return VOD_BAD_DATA;
			}

			read_be16(cur_pos, unit_size);

			if ((size_t)(end_pos - cur_pos) < unit_size)
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"hevc_parser_parse_extra_data: extra data overflow while reading unit data");
				return VOD_BAD_DATA;
			}

			if (unit_size < HEVC_NAL_HEADER_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
					"hevc_parser_parse_extra_data: unit smaller than header size");
				return VOD_BAD_DATA;
			}

			// skip the nal unit type
			nal_type = *cur_pos;
			cur_pos += HEVC_NAL_HEADER_SIZE;
			unit_size -= HEVC_NAL_HEADER_SIZE;

			rc = avc_hevc_parser_emulation_prevention_decode(ctx->request_context, &reader, cur_pos, unit_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			switch ((nal_type >> 1) & 0x3f)
			{
			case HEVC_NAL_SPS_NUT:
				rc = hevc_parser_seq_parameter_set_rbsp(ctx, &reader);
				if (rc != VOD_OK)
				{
					return rc;
				}
				break;

			case HEVC_NAL_PPS_NUT:
				rc = hevc_parser_pic_parameter_set_rbsp(ctx, &reader);
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
hevc_parser_skip_ref_pic_lists_modification(
	bit_reader_state_t* reader,
	uint32_t slice_type,
	uint32_t* num_ref_idx,
	int num_pic_total_curr)
{
	uint32_t ref_pic_list_modification_flag_l0;
	uint32_t ref_pic_list_modification_flag_l1;
	uint32_t i;

	ref_pic_list_modification_flag_l0 = bit_read_stream_get_one(reader);
	if (ref_pic_list_modification_flag_l0)
	{
		for (i = 0; i < num_ref_idx[0] && !reader->stream.eof_reached; i++)
		{
			bit_read_stream_skip(reader, avc_hevc_parser_ceil_log2(num_pic_total_curr));			// list_entry_l0[i]
		}
	}

	if (slice_type == HEVC_SLICE_B)
	{
		ref_pic_list_modification_flag_l1 = bit_read_stream_get_one(reader);
		if (ref_pic_list_modification_flag_l1)
		{
			for (i = 0; i <= num_ref_idx[1] && !reader->stream.eof_reached; i++)
			{
				bit_read_stream_skip(reader, avc_hevc_parser_ceil_log2(num_pic_total_curr));		// list_entry_l1[i]
			}
		}
	}
}

static void
hevc_parser_skip_pred_weight_table(
	bit_reader_state_t* reader,
	uint8_t slice_type,
	uint32_t* num_ref_idx,
	int chroma_array_type)
{
	uint32_t luma_weight_l0_flag;
	uint32_t chroma_weight_l0_flag;
	uint32_t luma_weight_l1_flag;
	uint32_t chroma_weight_l1_flag;
	uint32_t j;
	uint32_t i;

	bit_read_stream_skip_unsigned_exp(reader);			// luma_log2_weight_denom 
	if (chroma_array_type != 0)
	{
		bit_read_stream_skip_signed_exp(reader);		// delta_chroma_log2_weight_denom 
	}

	luma_weight_l0_flag = 0;
	chroma_weight_l0_flag = 0;
	for (i = 0; i < num_ref_idx[0] && !reader->stream.eof_reached; i++)
	{
		luma_weight_l0_flag |= bit_read_stream_get_one(reader) << i;
	}

	if (chroma_array_type != 0)
	{
		for (i = 0; i < num_ref_idx[0] && !reader->stream.eof_reached; i++)
		{
			chroma_weight_l0_flag |= bit_read_stream_get_one(reader) << i;
		}
	}

	for (i = 0; i < num_ref_idx[0] && !reader->stream.eof_reached; i++)
	{
		if ((luma_weight_l0_flag >> i) & 1)
		{
			bit_read_stream_skip_signed_exp(reader);	// delta_luma_weight_l0[i] 
			bit_read_stream_skip_signed_exp(reader);	// luma_offset_l0[i] 
		}

		if ((chroma_weight_l0_flag >> i) & 1)
		{
			for (j = 0; j < 2; j++)
			{
				bit_read_stream_skip_signed_exp(reader);	// delta_chroma_weight_l0[i][j] 
				bit_read_stream_skip_signed_exp(reader);	// delta_chroma_offset_l0[i][j] 
			}
		}
	}
	if (slice_type == HEVC_SLICE_B)
	{
		luma_weight_l1_flag = 0;
		chroma_weight_l1_flag = 0;
		for (i = 0; i < num_ref_idx[1] && !reader->stream.eof_reached; i++)
		{
			luma_weight_l1_flag |= bit_read_stream_get_one(reader) << i;
		}

		if (chroma_array_type != 0)
		{
			for (i = 0; i < num_ref_idx[1] && !reader->stream.eof_reached; i++)
			{
				chroma_weight_l1_flag |= bit_read_stream_get_one(reader) << i;
			}
		}

		for (i = 0; i < num_ref_idx[1] && !reader->stream.eof_reached; i++)
		{
			if ((luma_weight_l1_flag >> i) & 1)
			{
				bit_read_stream_skip_signed_exp(reader);	// delta_luma_weight_l1[i] 
				bit_read_stream_skip_signed_exp(reader);	// luma_offset_l1[i] 
			}

			if ((chroma_weight_l1_flag >> i) & 1)
			{
				for (j = 0; j < 2; j++)
				{
					bit_read_stream_skip_signed_exp(reader);	// delta_chroma_weight_l1[i][j] 
					bit_read_stream_skip_signed_exp(reader);	// delta_chroma_offset_l1[i][j] 
				}
			}
		}
	}
}

static unsigned
hevc_parser_st_rps_frame_nb_refs(hevc_short_term_rps_t* rps)
{
	unsigned ret = 0;
	unsigned i;

	for (i = 0; i < rps->num_delta_pocs; i++)
	{
		ret += rps->used[i];
	}

	return ret;
}

vod_status_t
hevc_parser_get_slice_header_size(
	void* context,
	const u_char* buffer,
	uint32_t size,
	uint32_t* result)
{
	hevc_short_term_rps_t* st_rps = NULL;
	hevc_short_term_rps_t st_rps_buf;
	bit_reader_state_t reader;
	avc_hevc_parse_ctx_t* ctx = context;
	const u_char* start_pos;
	hevc_sps_t* sps;
	hevc_pps_t* pps;
	uint32_t slice_deblocking_filter_disabled_flag;
	uint32_t slice_segment_header_extension_length;
	uint32_t num_ref_idx_active_override_flag;
	uint32_t first_slice_segment_in_pic_flag;
	uint32_t short_term_ref_pic_set_sps_flag = 0;
	uint32_t slice_temporal_mvp_enabled_flag = 0;
	uint32_t deblocking_filter_override_flag = 0;
	uint32_t dependent_slice_segment_flag = 0;
	uint32_t slice_pic_parameter_set_id;
	uint32_t delta_poc_msb_present_flag;
	uint32_t short_term_ref_pic_set_idx;
	uint32_t collocated_from_l0_flag = 1;
	uint32_t lt_used_by_curr_pic_sum = 0;
	uint32_t slice_sao_chroma_flag = 0;
	uint32_t slice_sao_luma_flag = 0;
	uint32_t num_entry_point_offsets;
	uint32_t num_long_term_pics;
	uint32_t num_long_term_sps = 0;
	uint32_t offset_len_minus1;
	uint32_t num_ref_idx[2];
	uint32_t slice_type;
	uint32_t lt_idx_sps;
	uint8_t nal_unit_type;
	vod_status_t rc;
	unsigned ctb_sizey;
	unsigned pic_size_in_ctbsy;
	unsigned num_pic_total_curr;
	unsigned i;

	rc = avc_hevc_parser_emulation_prevention_decode(
		ctx->request_context,
		&reader,
		buffer + HEVC_NAL_HEADER_SIZE,
		size - HEVC_NAL_HEADER_SIZE);
	if (rc != VOD_OK)
	{
		return rc;
	}

	start_pos = reader.stream.cur_pos;

	nal_unit_type = (buffer[0] >> 1) & 0x3f;

	first_slice_segment_in_pic_flag = bit_read_stream_get_one(&reader);
	if (nal_unit_type >= HEVC_NAL_BLA_W_LP && nal_unit_type <= HEVC_NAL_RSV_IRAP_VCL23)
	{
		bit_read_stream_get_one(&reader);				// no_output_of_prior_pics_flag
	}

	slice_pic_parameter_set_id = bit_read_stream_get_unsigned_exp(&reader);
	if (slice_pic_parameter_set_id >= ctx->pps.nelts)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_get_slice_header_size: invalid pps id %uD", slice_pic_parameter_set_id);
		return VOD_BAD_DATA;
	}

	pps = ((hevc_pps_t**)ctx->pps.elts)[slice_pic_parameter_set_id];
	if (pps == NULL)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_get_slice_header_size: non-existing pps id %uD", slice_pic_parameter_set_id);
		return VOD_BAD_DATA;
	}

	sps = pps->sps;

	if (!first_slice_segment_in_pic_flag)
	{
		if (pps->dependent_slice_segments_enabled_flag)
		{
			dependent_slice_segment_flag = bit_read_stream_get_one(&reader);
		}

		ctb_sizey = 1 << (sps->log2_min_luma_coding_block_size + sps->log2_diff_max_min_luma_coding_block_size);
		pic_size_in_ctbsy = vod_div_ceil(sps->pic_width_in_luma_samples, ctb_sizey) *
			vod_div_ceil(sps->pic_height_in_luma_samples, ctb_sizey);

		bit_read_stream_skip(&reader, avc_hevc_parser_ceil_log2(pic_size_in_ctbsy));		// slice_segment_address
	}

	if (!dependent_slice_segment_flag)
	{
		for (i = 0; i < pps->num_extra_slice_header_bits && !reader.stream.eof_reached; i++)
		{
			bit_read_stream_get_one(&reader);			// slice_reserved_flag[i]
		}
		slice_type = bit_read_stream_get_unsigned_exp(&reader);
		if (pps->output_flag_present_flag)
		{
			bit_read_stream_get_one(&reader);			// pic_output_flag
		}
		if (sps->separate_colour_plane_flag == 1)
		{
			bit_read_stream_skip(&reader, 2);			// colour_plane_id
		}

		if (nal_unit_type != HEVC_NAL_IDR_W_RADL && nal_unit_type != HEVC_NAL_IDR_N_LP)
		{
			bit_read_stream_skip(&reader, sps->log2_max_pic_order_cnt_lsb);			// slice_pic_order_cnt_lsb
			short_term_ref_pic_set_sps_flag = bit_read_stream_get_one(&reader);
			if (!short_term_ref_pic_set_sps_flag)
			{
				rc = hevc_parser_skip_st_ref_pic_set(ctx, &reader, sps, sps->num_short_term_ref_pic_sets, &st_rps_buf);
				if (rc != VOD_OK)
				{
					return rc;
				}
				st_rps = &st_rps_buf;
			}
			else
			{
				if (sps->num_short_term_ref_pic_sets > 1)
				{
					short_term_ref_pic_set_idx = bit_read_stream_get(&reader, avc_hevc_parser_ceil_log2(sps->num_short_term_ref_pic_sets));
				}
				else if (sps->num_short_term_ref_pic_sets > 0)
				{
					short_term_ref_pic_set_idx = 0;
				}
				else
				{
					vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
						"hevc_parser_get_slice_header_size: num_short_term_ref_pic_sets is zero");
					return VOD_BAD_DATA;
				}

				if (short_term_ref_pic_set_idx >= sps->num_short_term_ref_pic_sets)
				{
					vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
						"hevc_parser_get_slice_header_size: invalid short_term_ref_pic_set_idx %uD",
						sps->num_short_term_ref_pic_sets);
					return VOD_BAD_DATA;
				}				

				st_rps = &sps->st_rps[short_term_ref_pic_set_idx];
			}

			if (sps->long_term_ref_pics_present_flag)
			{
				if (sps->num_long_term_ref_pics_sps > 0)
				{
					num_long_term_sps = bit_read_stream_get_unsigned_exp(&reader);
				}
				num_long_term_pics = bit_read_stream_get_unsigned_exp(&reader);
				for (i = 0; i < num_long_term_sps + num_long_term_pics && !reader.stream.eof_reached; i++)
				{
					if (i < num_long_term_sps)
					{
						if (sps->num_long_term_ref_pics_sps > 1)
						{
							lt_idx_sps = bit_read_stream_get(&reader, 
								avc_hevc_parser_ceil_log2(sps->num_long_term_ref_pics_sps));
							lt_used_by_curr_pic_sum += (sps->used_by_curr_pic_lt_sps_flag >> lt_idx_sps) & 1;
						}
					}
					else
					{
						bit_read_stream_skip(&reader, sps->log2_max_pic_order_cnt_lsb);		// poc_lsb_lt[i]
						lt_used_by_curr_pic_sum += bit_read_stream_get_one(&reader);	// used_by_curr_pic_lt_flag[i]
					}

					delta_poc_msb_present_flag = bit_read_stream_get_one(&reader);
					if (delta_poc_msb_present_flag)
					{
						bit_read_stream_skip_unsigned_exp(&reader);		// delta_poc_msb_cycle_lt[i]
					}
				}
			}
			if (sps->sps_temporal_mvp_enabled_flag)
			{
				slice_temporal_mvp_enabled_flag = bit_read_stream_get_one(&reader);
			}
		}

		if (sps->sample_adaptive_offset_enabled_flag)
		{
			slice_sao_luma_flag = bit_read_stream_get_one(&reader);
			if (!sps->separate_colour_plane_flag || sps->chroma_format_idc != 3)
			{
				slice_sao_chroma_flag = bit_read_stream_get_one(&reader);
			}
		}

		if (slice_type == HEVC_SLICE_P || slice_type == HEVC_SLICE_B)
		{
			vod_memcpy(num_ref_idx, pps->num_ref_idx, sizeof(num_ref_idx));
			num_ref_idx_active_override_flag = bit_read_stream_get_one(&reader);
			if (num_ref_idx_active_override_flag)
			{
				num_ref_idx[0] = bit_read_stream_get_unsigned_exp(&reader) + 1;
				if (slice_type == HEVC_SLICE_B)
				{
					num_ref_idx[1] = bit_read_stream_get_unsigned_exp(&reader) + 1;
				}
			}

			if (pps->lists_modification_present_flag)
			{
				num_pic_total_curr = 
					(st_rps != NULL ? hevc_parser_st_rps_frame_nb_refs(st_rps) : 0) +
					lt_used_by_curr_pic_sum + 
					pps->pps_curr_pic_ref_enabled_flag;

				if (num_pic_total_curr > 1)
				{
					hevc_parser_skip_ref_pic_lists_modification(&reader, slice_type, num_ref_idx, num_pic_total_curr);
				}
			}
			if (slice_type == HEVC_SLICE_B)
			{
				bit_read_stream_get_one(&reader);		// mvd_l1_zero_flag
			}
			if (pps->cabac_init_present_flag)
			{
				bit_read_stream_get_one(&reader);		// cabac_init_flag
			}
			if (slice_temporal_mvp_enabled_flag)
			{
				if (slice_type == HEVC_SLICE_B)
				{
					collocated_from_l0_flag = bit_read_stream_get_one(&reader);
				}
				if ((collocated_from_l0_flag && num_ref_idx[0] > 1) ||
					(!collocated_from_l0_flag && num_ref_idx[1] > 1))
				{
					bit_read_stream_skip_unsigned_exp(&reader);		// collocated_ref_idx
				}
			}

			if ((pps->weighted_pred_flag && slice_type == HEVC_SLICE_P) ||
				(pps->weighted_bipred_flag && slice_type == HEVC_SLICE_B))
			{
				hevc_parser_skip_pred_weight_table(&reader, slice_type, num_ref_idx, !sps->separate_colour_plane_flag || sps->chroma_format_idc != 3);
			}

			bit_read_stream_skip_unsigned_exp(&reader);	// five_minus_max_num_merge_cand
			if (sps->motion_vector_resolution_control_idc == 2)
			{
				bit_read_stream_get_one(&reader);		// use_integer_mv_flag
			}
		}
		bit_read_stream_skip_signed_exp(&reader);		// slice_qp_delta
		if (pps->pps_slice_chroma_qp_offsets_present_flag)
		{
			bit_read_stream_skip_signed_exp(&reader);	// slice_cb_qp_offset
			bit_read_stream_skip_signed_exp(&reader);	// slice_cr_qp_offset
		}
		if (pps->pps_slice_act_qp_offsets_present_flag)
		{
			bit_read_stream_skip_signed_exp(&reader);	// slice_act_y_qp_offset
			bit_read_stream_skip_signed_exp(&reader);	// slice_act_cb_qp_offset
			bit_read_stream_skip_signed_exp(&reader);	// slice_act_cr_qp_offset
		}
		if (pps->chroma_qp_offset_list_enabled_flag)
		{
			bit_read_stream_get_one(&reader);			// cu_chroma_qp_offset_enabled_flag
		}
		if (pps->deblocking_filter_override_enabled_flag)
		{
			deblocking_filter_override_flag = bit_read_stream_get_one(&reader);
		}
		if (deblocking_filter_override_flag)
		{
			slice_deblocking_filter_disabled_flag = bit_read_stream_get_one(&reader);
			if (!slice_deblocking_filter_disabled_flag)
			{
				bit_read_stream_skip_signed_exp(&reader);	// slice_beta_offset_div2
				bit_read_stream_skip_signed_exp(&reader);	// slice_tc_offset_div2
			}
		}
		else
		{
			slice_deblocking_filter_disabled_flag = pps->pps_deblocking_filter_disabled_flag;
		}
		if (pps->pps_loop_filter_across_slices_enabled_flag &&
			(slice_sao_luma_flag || slice_sao_chroma_flag ||
				!slice_deblocking_filter_disabled_flag))
		{
			bit_read_stream_get_one(&reader);			// slice_loop_filter_across_slices_enabled_flag
		}
	}
	if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag)
	{
		num_entry_point_offsets = bit_read_stream_get_unsigned_exp(&reader);
		if (num_entry_point_offsets > 0)
		{
			offset_len_minus1 = bit_read_stream_get_unsigned_exp(&reader);
			for (i = 0; i < num_entry_point_offsets && !reader.stream.eof_reached; i++)
			{
				bit_read_stream_skip(&reader, offset_len_minus1 + 1);		// entry_point_offset_minus1[i]
			}
		}
	}
	if (pps->slice_segment_header_extension_present_flag)
	{
		slice_segment_header_extension_length = bit_read_stream_get_unsigned_exp(&reader);
		for (i = 0; i < slice_segment_header_extension_length && !reader.stream.eof_reached; i++)
		{
			bit_read_stream_skip(&reader, 8);			// slice_segment_header_extension_data_byte[ i ]
		}
	}

	if (bit_read_stream_get_one(&reader) != 1)			// alignment_bit_equal_to_one
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_get_slice_header_size: invalid alignment one bit");
		return VOD_BAD_DATA;
	}

	while (reader.cur_bit > 0 && !reader.stream.eof_reached)
	{
		if (bit_read_stream_get_one(&reader) != 0)			// alignment_bit_equal_to_zero
		{
			vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
				"hevc_parser_get_slice_header_size: invalid alignment zero bit");
			return VOD_BAD_DATA;
		}
	}

	if (reader.stream.eof_reached)
	{
		vod_log_error(VOD_LOG_ERR, ctx->request_context->log, 0,
			"hevc_parser_get_slice_header_size: bit stream overflow");
		return VOD_BAD_DATA;
	}

	*result = HEVC_NAL_HEADER_SIZE + (reader.stream.cur_pos - start_pos);

	if (start_pos != buffer + HEVC_NAL_HEADER_SIZE)
	{
		*result += avc_hevc_parser_emulation_prevention_encode_bytes(
			start_pos,
			reader.stream.cur_pos);
	}

	return VOD_OK;
}

vod_status_t
hevc_parser_is_slice(void* context, uint8_t nal_type, bool_t* is_slice)
{
	nal_type = (nal_type >> 1) & 0x3f;
	*is_slice = (nal_type <= HEVC_NAL_RASL_R) ||
		(nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_CRA_NUT);
	return VOD_OK;
}

uint8_t
hevc_parser_get_transfer_characteristics(
	void* context)
{
	avc_hevc_parse_ctx_t* ctx = context;
	hevc_sps_t** cur = (hevc_sps_t**)ctx->sps.elts;
	hevc_sps_t** end = cur + ctx->sps.nelts;
	hevc_sps_t* sps;

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
