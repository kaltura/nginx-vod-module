#include "hds_amf0_encoder.h"
#include "../write_stream.h"

// amf types
#define AMF0_TYPE_NUMBER		(0x00)
#define AMF0_TYPE_BOOLEAN		(0x01)
#define AMF0_TYPE_STRING		(0x02)
#define AMF0_TYPE_ECMA_ARRAY	(0x08)
#define AMF0_TYPE_OBJECT_END	(0x09)

static const vod_str_t amf0_on_metadata = vod_string("onMetaData");

// field name strings
#define AMF0_FIELD(group, name, type) static const vod_str_t amf0_ ## name = { sizeof(#name) - 1, (u_char *) #name };

#include "hds_amf0_fields_x.h"

#undef AMF0_FIELD

// field counts
#define AMF0_COMMON     0x01
#define AMF0_VIDEO    0x0100
#define AMF0_AUDIO	0x010000

#define AMF0_COMMON_FIELDS_COUNT (amf0_field_count & 0xFF)
#define AMF0_VIDEO_FIELDS_COUNT ((amf0_field_count >> 8) & 0xFF)
#define AMF0_AUDIO_FIELDS_COUNT ((amf0_field_count >> 16) & 0xFF)

#define AMF0_FIELD(group, name, type)    group + 

static const int amf0_field_count =
#include "hds_amf0_fields_x.h"
0;

#undef AMF0_FIELD

static vod_inline u_char*
hds_amf0_append_number(u_char* p, double value)
{
	u_char* v = (u_char*)&value + sizeof(double) - 1;
	*p++ = AMF0_TYPE_NUMBER;
	*p++ = *v--;	*p++ = *v--;	*p++ = *v--;	*p++ = *v--;
	*p++ = *v--;	*p++ = *v--;	*p++ = *v--;	*p++ = *v--;
	return p;
}

static vod_inline u_char*
hds_amf0_append_boolean(u_char* p, bool_t value)
{
	*p++ = AMF0_TYPE_BOOLEAN;
	*p++ = (value ? 0x01 : 0x00);
	return p;
}

static vod_inline u_char*
hds_amf0_append_raw_string(u_char* p, const vod_str_t* str)
{
	*p++ = (str->len >> 8) & 0xFF;
	*p++ = str->len & 0xFF;
	p = vod_copy(p, str->data, str->len);
	return p;
}

static vod_inline u_char*
hds_amf0_append_string(u_char* p, const vod_str_t* str)
{
	*p++ = AMF0_TYPE_STRING;
	return hds_amf0_append_raw_string(p, str);
}

static vod_inline u_char*
hds_amf0_append_array_header(u_char* p, uint32_t count)
{
	*p++ = AMF0_TYPE_ECMA_ARRAY;
	write_be32(p, count);
	return p;
}

static vod_inline u_char*
hds_amf0_append_array_number_value(u_char* p, const vod_str_t* key, double value)
{
	p = hds_amf0_append_raw_string(p, key);
	p = hds_amf0_append_number(p, value);
	return p;
}

static vod_inline u_char*
hds_amf0_append_array_boolean_value(u_char* p, const vod_str_t* key, bool_t value)
{
	p = hds_amf0_append_raw_string(p, key);
	p = hds_amf0_append_boolean(p, value);
	return p;
}

static vod_inline u_char*
hds_amf0_append_array_end(u_char* p)
{
	*p++ = 0;
	*p++ = 0;
	*p++ = AMF0_TYPE_OBJECT_END;
	return p;
}

static void
hds_get_max_duration(media_track_t** tracks, uint64_t* duration, uint32_t* timescale)
{
	if (tracks[MEDIA_TYPE_VIDEO] != NULL &&
		(tracks[MEDIA_TYPE_AUDIO] == NULL ||
		tracks[MEDIA_TYPE_VIDEO]->media_info.duration * tracks[MEDIA_TYPE_AUDIO]->media_info.timescale >
		tracks[MEDIA_TYPE_AUDIO]->media_info.duration * tracks[MEDIA_TYPE_VIDEO]->media_info.timescale))
	{
		*duration = tracks[MEDIA_TYPE_VIDEO]->media_info.duration;
		*timescale = tracks[MEDIA_TYPE_VIDEO]->media_info.timescale;
	}
	else
	{
		*duration = tracks[MEDIA_TYPE_AUDIO]->media_info.duration;
		*timescale = tracks[MEDIA_TYPE_AUDIO]->media_info.timescale;
	}
}

static u_char*
hds_amf0_write_metadata(u_char* p, media_set_t* media_set, media_track_t** tracks)
{
	media_info_t* media_info;
	uint64_t file_size = 0;
	uint64_t duration;
	uint32_t timescale;
	uint32_t count;
	uint8_t sound_format;

	count = AMF0_COMMON_FIELDS_COUNT;
	if (tracks[MEDIA_TYPE_VIDEO] != NULL)
	{
		count += AMF0_VIDEO_FIELDS_COUNT;
	}
	if (tracks[MEDIA_TYPE_AUDIO] != NULL)
	{
		count += AMF0_AUDIO_FIELDS_COUNT;
	}

	if (media_set->type != MEDIA_SET_VOD)
	{
		count--;		// duration is returned only for vod
	}

	p = hds_amf0_append_string(p, &amf0_on_metadata);
	p = hds_amf0_append_array_header(p, count);
	if (media_set->type == MEDIA_SET_VOD)
	{
		hds_get_max_duration(tracks, &duration, &timescale);
		p = hds_amf0_append_array_number_value(p, &amf0_duration, (double)duration / (double)timescale);
	}
	if (tracks[MEDIA_TYPE_VIDEO] != NULL)
	{
		media_info = &tracks[MEDIA_TYPE_VIDEO]->media_info;
		p = hds_amf0_append_array_number_value(p, &amf0_width, media_info->u.video.width);
		p = hds_amf0_append_array_number_value(p, &amf0_height, media_info->u.video.height);
		p = hds_amf0_append_array_number_value(p, &amf0_videodatarate, (double)media_info->bitrate / 1000.0);
		p = hds_amf0_append_array_number_value(p, &amf0_framerate, (double)media_info->timescale / (double)media_info->min_frame_duration);
		p = hds_amf0_append_array_number_value(p, &amf0_videocodecid, CODEC_ID_AVC);
		file_size += tracks[MEDIA_TYPE_VIDEO]->total_frames_size;
	}
	if (tracks[MEDIA_TYPE_AUDIO] != NULL)
	{
		media_info = &tracks[MEDIA_TYPE_AUDIO]->media_info;
		p = hds_amf0_append_array_number_value(p, &amf0_audiodatarate, (double)media_info->bitrate / 1000.0);
		p = hds_amf0_append_array_number_value(p, &amf0_audiosamplerate, media_info->u.audio.sample_rate);
		p = hds_amf0_append_array_number_value(p, &amf0_audiosamplesize, media_info->u.audio.bits_per_sample);
		p = hds_amf0_append_array_boolean_value(p, &amf0_stereo, media_info->u.audio.channels > 1);
		switch (media_info->codec_id)
		{
		case VOD_CODEC_ID_MP3:
			sound_format = SOUND_FORMAT_MP3;
			break;

		default:
			sound_format = SOUND_FORMAT_AAC;
			break;
		}
		p = hds_amf0_append_array_number_value(p, &amf0_audiocodecid, sound_format);
		file_size += tracks[MEDIA_TYPE_AUDIO]->total_frames_size;
	}
	p = hds_amf0_append_array_number_value(p, &amf0_filesize, file_size);
	p = hds_amf0_append_array_end(p);
	return p;
}

u_char*
hds_amf0_write_base64_metadata(u_char* p, u_char* temp_buffer, media_set_t* media_set, media_track_t** tracks)
{
	vod_str_t binary;
	vod_str_t base64;

	binary.data = temp_buffer;
	binary.len = hds_amf0_write_metadata(temp_buffer, media_set, tracks) - binary.data;

	base64.data = p;

	vod_encode_base64(&base64, &binary);

	return p + base64.len;
}
