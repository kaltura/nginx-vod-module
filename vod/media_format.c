#include "media_format.h"

static uint16_t channel_config_to_channel_count[] = {
	0, // defined in AOT Specific Config
	1, // front - center
	2, // front - left, front - right
	3, // front - center, front - left, front - right
	4, // front - center, front - left, front - right, back - center
	5, // front - center, front - left, front - right, back - left, back - right
	6, // front - center, front - left, front - right, back - left, back - right, LFE - channel
	8, // front - center, front - left, front - right, side - left, side - right, back - left, back - right, LFE - channel
};

vod_status_t
media_format_finalize_track(
	request_context_t* request_context, 
	int parse_type,
	media_info_t* media_info)
{
	codec_config_get_nal_units_t get_nal_units = NULL;
	vod_status_t rc = VOD_OK;
	u_char* new_extra_data;
	uint8_t channel_config;

	if (media_info->media_type == MEDIA_TYPE_AUDIO)
	{
		// always save the audio extra data to support audio filtering
		parse_type |= PARSE_FLAG_EXTRA_DATA;

		// derive the channel count from the codec config when possible
		//	the channel count in the stsd atom is occasionally wrong
		channel_config = media_info->u.audio.codec_config.channel_config;
		if (channel_config > 0 && channel_config < vod_array_entries(channel_config_to_channel_count))
		{
			media_info->u.audio.channels = channel_config_to_channel_count[channel_config];
		}
	}

	// get the codec name
	if ((parse_type & PARSE_FLAG_CODEC_NAME) != 0)
	{
		media_info->codec_name.data = vod_alloc(request_context->pool, MAX_CODEC_NAME_SIZE);
		if (media_info->codec_name.data == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"media_format_finalize_track: failed to allocate codec name");
			return VOD_ALLOC_FAILED;
		}

		switch (media_info->media_type)
		{
		case MEDIA_TYPE_VIDEO:
			rc = codec_config_get_video_codec_name(request_context, media_info);
			break;

		case MEDIA_TYPE_AUDIO:
			rc = codec_config_get_audio_codec_name(request_context, media_info);
			break;
		}

		if (rc != VOD_OK)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"media_format_finalize_track: failed to get codec name");
			return rc;
		}
	}

	// parse / copy the extra data
	if ((parse_type & (PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_EXTRA_DATA_SIZE)) != 0)
	{
		if ((parse_type & PARSE_FLAG_EXTRA_DATA_PARSE) != 0 &&
			media_info->media_type == MEDIA_TYPE_VIDEO)
		{
			// extract the nal units
			switch (media_info->codec_id)
			{
			case VOD_CODEC_ID_AVC:
				get_nal_units = codec_config_avcc_get_nal_units;
				break;

			case VOD_CODEC_ID_HEVC:
				get_nal_units = codec_config_hevc_get_nal_units;
				break;
			}

			rc = get_nal_units(
				request_context,
				&media_info->extra_data,
				(parse_type & PARSE_FLAG_EXTRA_DATA) == 0,
				&media_info->u.video.nal_packet_size_length,
				&media_info->extra_data);
			if (rc != VOD_OK)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"media_format_finalize_track: get_nal_units failed %i", rc);
				return rc;
			}
		}
		else if ((parse_type & PARSE_FLAG_EXTRA_DATA) != 0)
		{
			// copy the extra data, we should not reference the moov buffer after we finish parsing
			new_extra_data = vod_alloc(request_context->pool, media_info->extra_data.len + VOD_BUFFER_PADDING_SIZE);
			if (new_extra_data == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"media_format_finalize_track: vod_alloc failed");
				return VOD_ALLOC_FAILED;
			}
			vod_memcpy(new_extra_data, media_info->extra_data.data, media_info->extra_data.len);
			vod_memzero(new_extra_data + media_info->extra_data.len, VOD_BUFFER_PADDING_SIZE);
			media_info->extra_data.data = new_extra_data;
		}
		else
		{
			media_info->extra_data.data = NULL;
		}
	}
	else
	{
		media_info->extra_data.data = NULL;
		media_info->extra_data.len = 0;
	}

	return VOD_OK;
}
