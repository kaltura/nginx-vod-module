#include "media_format.h"
#include "avc_hevc_parser.h"
#include "avc_parser.h"
#include "hevc_parser.h"

vod_status_t
media_format_finalize_track(
	request_context_t* request_context, 
	int parse_type,
	media_info_t* media_info)
{
	codec_config_get_nal_units_t get_nal_units = NULL;
	vod_status_t rc = VOD_OK;
	u_char* new_extra_data;
	void* parser_ctx;

	switch (media_info->media_type)
	{
	case MEDIA_TYPE_AUDIO:
		// always save the audio extra data to support audio filtering
		parse_type |= PARSE_FLAG_EXTRA_DATA;
		break;

	case MEDIA_TYPE_VIDEO:
		if ((parse_type & PARSE_FLAG_CODEC_TRANSFER_CHAR) == 0)
		{
			break;
		}

		if (media_info->codec_id != VOD_CODEC_ID_AVC && media_info->codec_id != VOD_CODEC_ID_HEVC)
		{
			break;
		}

		rc = avc_hevc_parser_init_ctx(
			request_context,
			&parser_ctx);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (media_info->codec_id == VOD_CODEC_ID_AVC)
		{
			rc = avc_parser_parse_extra_data(
				parser_ctx,
				&media_info->extra_data,
				NULL,
				NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}

			media_info->u.video.transfer_characteristics = avc_parser_get_transfer_characteristics(
				parser_ctx);
		}
		else
		{
			rc = hevc_parser_parse_extra_data(
				parser_ctx,
				&media_info->extra_data,
				NULL,
				NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}

			media_info->u.video.transfer_characteristics = hevc_parser_get_transfer_characteristics(
				parser_ctx);
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

			if (get_nal_units != NULL)
			{
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
