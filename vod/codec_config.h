#ifndef __CODEC_CONFIG_H__
#define __CODEC_CONFIG_H__

// includes
#include "common.h"

// typedefs
struct media_info_s;

typedef vod_status_t (*codec_config_get_nal_units_t)(
	request_context_t* request_context,
	vod_str_t* extra_data,
	bool_t size_only,
	uint32_t* nal_packet_size_length,
	vod_str_t* result);

typedef struct
{
	u_char version;
	u_char profile;
	u_char compatibility;
	u_char level;
	u_char nula_length_size;
} avcc_config_t;

typedef struct
{
	uint8_t configurationVersion;
	uint8_t profile_space;
	uint8_t tier_flag;
	uint8_t profile_idc;
	uint32_t general_profile_compatibility_flags;
	uint8_t progressive_source_flag;
	uint8_t interlaced_source_flag;
	uint8_t non_packed_constraint_flag;
	uint8_t frame_only_constraint_flag;
	/*only lowest 44 bits used*/
	uint64_t constraint_indicator_flags;
	uint8_t level_idc;
	uint16_t min_spatial_segmentation_idc;

	uint8_t parallelismType;
	uint8_t chromaFormat;
	uint8_t luma_bit_depth;
	uint8_t chroma_bit_depth;
	uint16_t avgFrameRate;
	uint8_t constantFrameRate;
	uint8_t numTemporalLayers;
	uint8_t temporalIdNested;

	uint8_t nal_unit_size;

	//set by libisomedia at import/export time
	bool_t is_shvc;

	//used in SHVC config
	bool_t complete_representation;
	bool_t non_hevc_base_layer;
	uint8_t num_layers;
	uint16_t scalability_mask;
} hevc_config_t;

typedef struct {
	uint8_t object_type;
	uint8_t sample_rate_index;
	uint8_t channel_config;
} mp4a_config_t;

// functions
vod_status_t codec_config_hevc_get_nal_units(
	request_context_t* request_context,
	vod_str_t* extra_data,
	bool_t size_only,
	uint32_t* nal_packet_size_length,
	vod_str_t* result);

vod_status_t codec_config_avcc_get_nal_units(
	request_context_t* request_context,
	vod_str_t* extra_data,
	bool_t size_only,
	uint32_t* nal_packet_size_length,
	vod_str_t* result);

// get codec name according to http://tools.ietf.org/html/rfc6381
vod_status_t codec_config_get_video_codec_name(request_context_t* request_context, struct media_info_s* media_info);
vod_status_t codec_config_get_audio_codec_name(request_context_t* request_context, struct media_info_s* media_info);

vod_status_t codec_config_mp4a_config_parse(
	request_context_t* request_context,
	vod_str_t* extra_data, 
	struct media_info_s* media_info);

vod_status_t codec_config_hevc_config_parse(
	request_context_t* request_context,
	vod_str_t* extra_data,
	hevc_config_t* cfg,
	const u_char** end_pos);

#endif // __CODEC_CONFIG_H__
