#include "mss_packager.h"

// constants
#define MSS_TIMESCALE (10000000)

// manifest constants
#define MSS_MANIFEST_HEADER \
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"	\
	"<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"0\" Duration=\"%uL\">\n"

#define MSS_STREAM_INDEX_HEADER \
	"  <StreamIndex Type=\"%s\" QualityLevels=\"%uD\" Chunks=\"%uD\" Url=\"QualityLevels({bitrate})/Fragments(%s={start time})\">\n"

#define MSS_VIDEO_QUALITY_LEVEL_HEADER \
	"    <QualityLevel Index=\"%uD\" Bitrate=\"%uD\" FourCC=\"H264\" MaxWidth=\"%uD\" MaxHeight=\"%uD\" " \
	"CodecPrivateData=\""

#define MSS_AUDIO_QUALITY_LEVEL_HEADER \
	"    <QualityLevel Index=\"%uD\" Bitrate=\"%uD\" FourCC=\"AACL\" SamplingRate=\"%uD\"" \
	" Channels=\"%uD\" BitsPerSample=\"%uD\" PacketSize=\"%uD\" AudioTag=\"255\" CodecPrivateData=\""

#define MSS_QUALITY_LEVEL_FOOTER "\"></QualityLevel>\n"

#define MSS_CHUNK_TAG \
	"    <c n=\"%uD\" d=\"%uD\"></c>\n"

#define MSS_STREAM_INDEX_FOOTER \
	"  </StreamIndex>\n"

#define MSS_MANIFEST_FOOTER \
	"</SmoothStreamingMedia>\n"

// typedefs
typedef struct {
	u_char uuid[16];
	u_char version[1];
	u_char flags[3];
	u_char timestamp[8];
	u_char duration[8];
} uuid_tfxd_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char track_id[4];
	u_char default_sample_flags[4];
} tfhd_atom_t;

// constants
static const uint8_t tfxd_uuid[] = {
	0x6d, 0x1d, 0x9b, 0x05, 0x42, 0xd5, 0x44, 0xe6,
	0x80, 0xe2, 0x14, 0x1d, 0xaf, 0xf7, 0x57, 0xb2
};

static u_char*
mss_append_hex_string(u_char* p, const u_char* buffer, uint32_t buffer_size)
{
	const u_char* buffer_end = buffer + buffer_size;
	static const u_char hex_chars[] = "0123456789ABCDEF";

	for (; buffer < buffer_end; buffer++)
	{
		*p++ = hex_chars[*buffer >> 4];
		*p++ = hex_chars[*buffer & 0x0F];
	}
	return p;
}

static u_char*
mss_write_manifest_chunks(u_char* p, mpeg_stream_metadata_t* cur_stream, uint32_t segment_duration)
{
	input_frame_t* last_frame = cur_stream->frames + cur_stream->frame_count;
	input_frame_t* cur_frame;
	uint32_t segment_index = 0;
	uint64_t accum_duration = 0;
	uint64_t segment_start = 0;
	uint64_t segment_limit;

	segment_start = 0;
	segment_limit = rescale_time(segment_duration, 1000, cur_stream->media_info.timescale);
	for (cur_frame = cur_stream->frames; cur_frame < last_frame; cur_frame++)
	{
		accum_duration += cur_frame->duration;
		if (accum_duration >= segment_limit)
		{
			p = vod_sprintf(p, MSS_CHUNK_TAG, 
				segment_index, 
				(uint32_t)rescale_time(accum_duration - segment_start, cur_stream->media_info.timescale, MSS_TIMESCALE));
			segment_index++;
			segment_start = accum_duration;
			segment_limit = rescale_time((uint64_t)segment_duration * (segment_index + 1), 1000, cur_stream->media_info.timescale);
		}
	}

	if (accum_duration > segment_start)
	{
		p = vod_sprintf(p, MSS_CHUNK_TAG, 
			segment_index, 
			(uint32_t)rescale_time(accum_duration - segment_start, cur_stream->media_info.timescale, MSS_TIMESCALE));
	}

	return p;
}

bool_t 
mss_packager_compare_streams(void* context, const media_info_t* mi1, const media_info_t* mi2)
{
	uintptr_t bitrate_threshold = *(uintptr_t*)context;

	if (mi1->bitrate == 0 ||
		mi2->bitrate == 0 ||
		mi1->bitrate + bitrate_threshold <= mi2->bitrate ||
		mi2->bitrate + bitrate_threshold <= mi1->bitrate)
	{
		return FALSE;
	}

	if (mi1->media_type == MEDIA_TYPE_VIDEO)
	{
		return (mi1->u.video.width == mi2->u.video.width) &&
			(mi1->u.video.height == mi2->u.video.height);
	}

	return TRUE;
}

vod_status_t 
mss_packager_build_manifest(
	request_context_t* request_context, 
	uint32_t segment_duration, 
	mpeg_metadata_t* mpeg_metadata, 
	vod_str_t* result)
{
	mpeg_stream_metadata_t* cur_stream;
	mpeg_stream_metadata_t* streams_by_media_type[MEDIA_TYPE_COUNT];
	uint64_t duration_100ns;
	uint32_t media_type;
	uint32_t stream_index;
	uint32_t bitrate;
	size_t result_size;
	uint64_t segment_duration_ts;
	uint32_t segment_count;
	u_char* p;

	vod_memzero(&streams_by_media_type, sizeof(streams_by_media_type));

	// find the longest stream in each media type
	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		media_type = cur_stream->media_info.media_type;

		if (streams_by_media_type[media_type] == NULL ||
			streams_by_media_type[media_type]->media_info.duration * cur_stream->media_info.timescale <	
			cur_stream->media_info.duration * streams_by_media_type[media_type]->media_info.timescale)
		{
			streams_by_media_type[media_type] = cur_stream;
		}
	}

	// calculate the result size
	result_size = 
		sizeof(MSS_MANIFEST_HEADER) - 1 + VOD_INT64_LEN + 
		sizeof(MSS_MANIFEST_FOOTER);
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		if (streams_by_media_type[media_type] == NULL)
		{
			continue;
		}

		result_size +=
			sizeof(MSS_STREAM_INDEX_HEADER) - 1 + 2 * sizeof(MSS_STREAM_TYPE_VIDEO) + 2 * VOD_INT32_LEN +
			sizeof(MSS_STREAM_INDEX_FOOTER);

		segment_duration_ts = rescale_time(segment_duration, 1000, streams_by_media_type[media_type]->media_info.timescale);
		segment_count = streams_by_media_type[media_type]->total_frames_duration / segment_duration_ts + 1;
		result_size += segment_count * (sizeof(MSS_CHUNK_TAG) + 2 * VOD_INT32_LEN);
	}

	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		switch (cur_stream->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			result_size += 
				sizeof(MSS_VIDEO_QUALITY_LEVEL_HEADER) - 1 + 4 * VOD_INT32_LEN + cur_stream->media_info.extra_data_size * 2 + 
				sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1;
			break;

		case MEDIA_TYPE_AUDIO:
			result_size += 
				sizeof(MSS_AUDIO_QUALITY_LEVEL_HEADER) - 1 + 6 * VOD_INT32_LEN + cur_stream->media_info.extra_data_size * 2 +
				sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1;
			break;
		}
	}

	// allocate the result
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mss_packager_build_manifest: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	duration_100ns = rescale_time(mpeg_metadata->duration, mpeg_metadata->timescale, MSS_TIMESCALE);
	p = vod_sprintf(result->data, MSS_MANIFEST_HEADER, duration_100ns);

	if (streams_by_media_type[MEDIA_TYPE_VIDEO] != NULL)
	{
		p = vod_sprintf(p, 
			MSS_STREAM_INDEX_HEADER, 
			MSS_STREAM_TYPE_VIDEO,
			mpeg_metadata->stream_count[MEDIA_TYPE_VIDEO],
			(uint32_t)DIV_CEIL(
				rescale_time(streams_by_media_type[MEDIA_TYPE_VIDEO]->media_info.duration, 
					streams_by_media_type[MEDIA_TYPE_VIDEO]->media_info.timescale, 1000), 
				segment_duration),
			MSS_STREAM_TYPE_VIDEO);

		stream_index = 0;
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_VIDEO)
			{
				continue;
			}

			bitrate = cur_stream->media_info.bitrate;
			bitrate = MSS_ENCODE_INDEXES(bitrate, cur_stream->file_info.file_index, cur_stream->track_index);
			p = vod_sprintf(p, MSS_VIDEO_QUALITY_LEVEL_HEADER,
				stream_index++,
				bitrate,
				(uint32_t)cur_stream->media_info.u.video.width,
				(uint32_t)cur_stream->media_info.u.video.height);

			p = mss_append_hex_string(p, cur_stream->media_info.extra_data, cur_stream->media_info.extra_data_size);

			p = vod_copy(p, MSS_QUALITY_LEVEL_FOOTER, sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1);
		}

		p = mss_write_manifest_chunks(p, streams_by_media_type[MEDIA_TYPE_VIDEO], segment_duration);

		p = vod_copy(p, MSS_STREAM_INDEX_FOOTER, sizeof(MSS_STREAM_INDEX_FOOTER) - 1);
	}

	if (streams_by_media_type[MEDIA_TYPE_AUDIO] != NULL)
	{
		p = vod_sprintf(p, MSS_STREAM_INDEX_HEADER, 
			MSS_STREAM_TYPE_AUDIO,
			mpeg_metadata->stream_count[MEDIA_TYPE_AUDIO],
			(uint32_t)DIV_CEIL(
				rescale_time(streams_by_media_type[MEDIA_TYPE_AUDIO]->media_info.duration, 
					streams_by_media_type[MEDIA_TYPE_AUDIO]->media_info.timescale, 1000), 
				segment_duration),
			MSS_STREAM_TYPE_AUDIO);

		stream_index = 0;
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_AUDIO)
			{
				continue;
			}

			bitrate = cur_stream->media_info.bitrate;
			bitrate = MSS_ENCODE_INDEXES(bitrate, cur_stream->file_info.file_index, cur_stream->track_index);
			p = vod_sprintf(p, MSS_AUDIO_QUALITY_LEVEL_HEADER,
				stream_index++,
				bitrate,
				cur_stream->media_info.u.audio.sample_rate,
				(uint32_t)cur_stream->media_info.u.audio.channels,
				(uint32_t)cur_stream->media_info.u.audio.bits_per_sample,
				(uint32_t)cur_stream->media_info.u.audio.packet_size);

			p = mss_append_hex_string(p, cur_stream->media_info.extra_data, cur_stream->media_info.extra_data_size);

			p = vod_copy(p, MSS_QUALITY_LEVEL_FOOTER, sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1);
		}

		p = mss_write_manifest_chunks(p, streams_by_media_type[MEDIA_TYPE_AUDIO], segment_duration);

		p = vod_copy(p, MSS_STREAM_INDEX_FOOTER, sizeof(MSS_STREAM_INDEX_FOOTER) - 1);
	}

	p = vod_copy(p, MSS_MANIFEST_FOOTER, sizeof(MSS_MANIFEST_FOOTER) - 1);

	result->len = p - result->data;
	
	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mss_packager_build_manifest: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

static u_char*
mss_write_tfhd_atom(u_char* p, uint32_t track_id, uint32_t flags)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfhd_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'h', 'd');
	write_dword(p, 0x20);		// default sample flags
	write_dword(p, track_id);
	write_dword(p, flags);
	return p;
}

static u_char*
mss_write_uuid_tfxd_atom(u_char* p, mpeg_stream_metadata_t* stream_metadata)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(uuid_tfxd_atom_t);

	write_atom_header(p, atom_size, 'u', 'u', 'i', 'd');
	p = vod_copy(p, tfxd_uuid, sizeof(tfxd_uuid));
	write_dword(p, 0x01000000);		// version / flags
	write_qword(p, rescale_time(stream_metadata->first_frame_time_offset, stream_metadata->media_info.timescale, MSS_TIMESCALE));
	write_qword(p, rescale_time(stream_metadata->total_frames_duration, stream_metadata->media_info.timescale, MSS_TIMESCALE));
	return p;
}

vod_status_t
mss_packager_build_fragment_header(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size)
{
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	size_t mdat_atom_size;
	size_t trun_atom_size;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t result_size;
	u_char* p;

	// calculate sizes
	mdat_atom_size = ATOM_HEADER_SIZE + stream_metadata->total_frames_size;
	trun_atom_size = mp4_builder_get_trun_atom_size(stream_metadata->media_info.media_type, stream_metadata->frame_count);

	traf_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(tfhd_atom_t) +
		trun_atom_size +
		ATOM_HEADER_SIZE + sizeof(uuid_tfxd_atom_t);

	moof_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t)+
		traf_atom_size;

	result_size =
		moof_atom_size +
		ATOM_HEADER_SIZE;		// mdat

	*total_fragment_size = result_size + stream_metadata->total_frames_size;

	// head request optimization
	if (size_only)
	{
		return VOD_OK;
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mss_packager_build_fragment_header: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	p = result->data;

	// moof
	write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

	// moof.mfhd
	p = mp4_builder_write_mfhd_atom(p, segment_index);

	// moof.traf
	write_atom_header(p, traf_atom_size, 't', 'r', 'a', 'f');

	// moof.traf.tfhd
	switch (stream_metadata->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = mss_write_tfhd_atom(p, stream_metadata->media_info.track_id, 0x01010000);
		break;

	case MEDIA_TYPE_AUDIO:
		p = mss_write_tfhd_atom(p, stream_metadata->media_info.track_id, 0x02000000);
		break;
	}

	// moof.traf.trun
	last_frame = stream_metadata->frames + stream_metadata->frame_count;
	for (cur_frame = stream_metadata->frames; cur_frame < last_frame; cur_frame++)
	{
		cur_frame->duration = rescale_time(cur_frame->duration, stream_metadata->media_info.timescale, MSS_TIMESCALE);
	}

	p = mp4_builder_write_trun_atom(
		p,
		stream_metadata->media_info.media_type,
		stream_metadata->frames,
		stream_metadata->frame_count,
		moof_atom_size);

	p = mss_write_uuid_tfxd_atom(p, stream_metadata);

	// mdat
	write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');

	result->len = p - result->data;

	if (result->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mss_packager_build_fragment_header: result length %uz is different than allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}
