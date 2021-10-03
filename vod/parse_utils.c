#include "media_format.h"
#include "parse_utils.h"

static int
parse_utils_get_hex_char_value(int ch)
{
	if (ch >= '0' && ch <= '9')
	{
		return (ch - '0');
	}

	ch = (ch | 0x20);		// lower case

	if (ch >= 'a' && ch <= 'f')
	{
		return (ch - 'a' + 10);
	}

	return -1;
}

vod_status_t
parse_utils_parse_guid_string(vod_str_t* str, u_char* output)
{
	u_char* cur_pos;
	u_char* end_pos;
	u_char* output_end = output + VOD_GUID_SIZE;
	int c1;
	int c2;

	cur_pos = str->data;
	end_pos = cur_pos + str->len;
	while (cur_pos + 1 < end_pos)
	{
		if (*cur_pos == '-')
		{
			cur_pos++;
			continue;
		}

		if (output >= output_end)
		{
			return VOD_BAD_DATA;
		}

		c1 = parse_utils_get_hex_char_value(cur_pos[0]);
		c2 = parse_utils_get_hex_char_value(cur_pos[1]);
		if (c1 < 0 || c2 < 0)
		{
			return VOD_BAD_DATA;
		}

		*output++ = ((c1 << 4) | c2);
		cur_pos += 2;
	}

	if (output < output_end)
	{
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

static vod_status_t
parse_utils_base64_exact_decoded_length(vod_str_t* base64, size_t* decoded_len)
{
	size_t padding_size;
	u_char *cur_pos;

	if ((base64->len & 3) != 0)
	{
		return VOD_BAD_DATA;
	}

	padding_size = 0;
	for (cur_pos = base64->data + base64->len - 1; cur_pos >= base64->data; cur_pos--)
	{
		if (*cur_pos != '=')
		{
			break;
		}
		padding_size++;
	}

	if (padding_size > 2)
	{
		return VOD_BAD_DATA;
	}

	*decoded_len = (base64->len >> 2) * 3 - padding_size;
	return VOD_OK;
}

vod_status_t
parse_utils_parse_fixed_base64_string(vod_str_t* str, u_char* output, size_t output_size)
{
	vod_str_t output_str;
	vod_status_t rc;
	size_t decoded_len;

	rc = parse_utils_base64_exact_decoded_length(str, &decoded_len);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (decoded_len != output_size)
	{
		return VOD_BAD_DATA;
	}

	output_str.data = output;
	if (vod_decode_base64(&output_str, str) != VOD_OK)
	{
		return VOD_BAD_DATA;
	}

	if (output_str.len != output_size)
	{
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
parse_utils_parse_variable_base64_string(vod_pool_t* pool, vod_str_t* str, vod_str_t* result)
{
	result->data = vod_alloc(pool, vod_base64_decoded_length(str->len));
	if (result->data == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	if (vod_decode_base64(result, str) != VOD_OK)
	{
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

u_char*
parse_utils_extract_uint32_token(u_char* start_pos, u_char* end_pos, uint32_t* result)
{
	uint32_t value = 0;

	for (; start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9'; start_pos++)
	{
		value = value * 10 + *start_pos - '0';
	}
	*result = value;
	return start_pos;
}

u_char*
parse_utils_extract_track_tokens(u_char* start_pos, u_char* end_pos, track_mask_t* result)
{
	uint32_t stream_index;
	u_char* next_pos;
	int media_type;

	// by default use the first audio and first video streams
	if (start_pos >= end_pos || (*start_pos != 'a' && *start_pos != 'v'))
	{
		for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
		{
			vod_set_bit(result[media_type], 0);
		}
		return start_pos;
	}

	while (start_pos < end_pos)
	{
		switch (*start_pos)
		{
		case 'v':
			media_type = MEDIA_TYPE_VIDEO;
			break;

		case 'a':
			media_type = MEDIA_TYPE_AUDIO;
			break;

		default:
			return start_pos;
		}

		start_pos++;		// skip the v/a

		next_pos = parse_utils_extract_uint32_token(start_pos, end_pos, &stream_index);

		if (stream_index == 0)
		{
			// no index => all streams of the media type
			vod_track_mask_set_all_bits(result[media_type]);
		}
		else
		{
			vod_set_bit(result[media_type], stream_index - 1);
		}

		start_pos = next_pos;

		if (start_pos < end_pos && *start_pos == '-')
		{
			start_pos++;
		}
	}
	return start_pos;
}
