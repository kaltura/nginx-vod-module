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
	"    <c n=\"%uD\" d=\"%uL\"></c>\n"

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
mss_write_manifest_chunks(u_char* p, segment_durations_t* segment_durations)
{
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	uint32_t last_segment_index;
	uint32_t segment_index;

	for (cur_item = segment_durations->items; cur_item < last_item; cur_item++)
	{
		segment_index = cur_item->segment_index;
		last_segment_index = segment_index + cur_item->repeat_count;
		for (; segment_index < last_segment_index; segment_index++)
		{
			p = vod_sprintf(p, MSS_CHUNK_TAG, segment_index, rescale_time(cur_item->duration, segment_durations->timescale, MSS_TIMESCALE));
		}
	}

	return p;
}

static bool_t 
mss_packager_compare_tracks(uintptr_t bitrate_threshold, const media_info_t* mi1, const media_info_t* mi2)
{
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

static void 
mss_packager_remove_redundant_tracks(
	vod_uint_t duplicate_bitrate_threshold,
	media_set_t* media_set)
{
	media_track_t* track1;
	media_track_t* track2;
	media_track_t* last_track;
	uint32_t media_type1;
	uint32_t max_sample_rate = 0;
	uint32_t clip_index;
	bool_t remove_track;

	// find the max audio sample rate in the first clip
	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (track1 = media_set->filtered_tracks; track1 < last_track; track1++)
	{
		if (track1->media_info.media_type == MEDIA_TYPE_AUDIO &&
			track1->media_info.u.audio.sample_rate > max_sample_rate)
		{
			max_sample_rate = track1->media_info.u.audio.sample_rate;
		}
	}

	// remove duplicate tracks and tracks that have different sample rante
	for (track1 = media_set->filtered_tracks; track1 < last_track; track1++)
	{
		media_type1 = track1->media_info.media_type;
		if (media_type1 == MEDIA_TYPE_AUDIO &&
			track1->media_info.u.audio.sample_rate != max_sample_rate)
		{
			remove_track = TRUE;
		}
		else
		{
			remove_track = FALSE;
			for (track2 = media_set->filtered_tracks; track2 < track1; track2++)
			{
				if (media_type1 != track2->media_info.media_type)
				{
					continue;
				}

				if (!mss_packager_compare_tracks(
					duplicate_bitrate_threshold,
					&track1->media_info,
					&track2->media_info))
				{
					continue;
				}

				remove_track = TRUE;
				break;
			}
		}

		if (remove_track)
		{
			// remove the track from all clips
			media_set->track_count[media_type1]--;

			for (clip_index = 0; clip_index < media_set->clip_count; clip_index++)
			{
				track1[clip_index * media_set->total_track_count].media_info.media_type = MEDIA_TYPE_NONE;
			}
		}
	}
}

vod_status_t 
mss_packager_build_manifest(
	request_context_t* request_context, 
	mss_manifest_config_t* conf,
	segmenter_conf_t* segmenter_conf,
	media_set_t* media_set,
	size_t extra_tags_size,
	mss_write_tags_callback_t write_extra_tags,
	void* extra_tags_writer_context,
	vod_str_t* result)
{
	media_sequence_t* cur_sequence;
	media_track_t* last_track;
	media_track_t* cur_track;
	segment_durations_t segment_durations[MEDIA_TYPE_COUNT];
	uint64_t duration_100ns;
	uint32_t media_type;
	uint32_t stream_index;
	uint32_t bitrate;
	vod_status_t rc;
	size_t result_size;
	u_char* p;

	if (media_set->use_discontinuity)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mss_packager_build_manifest: discontinuity is not supported in MSS");
		return VOD_BAD_MAPPING;
	}

	// calculate the result size
	result_size = 
		sizeof(MSS_MANIFEST_HEADER) - 1 + VOD_INT64_LEN + 
		extra_tags_size +
		sizeof(MSS_MANIFEST_FOOTER);
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		if (media_set->track_count[media_type] == 0)
		{
			continue;
		}

		rc = segmenter_conf->get_segment_durations(
			request_context,
			segmenter_conf,
			media_set,
			NULL,
			media_type,
			&segment_durations[media_type]);
		if (rc != VOD_OK)
		{
			return rc;
		}

		result_size +=
			sizeof(MSS_STREAM_INDEX_HEADER) - 1 + 2 * sizeof(MSS_STREAM_TYPE_VIDEO) + 2 * VOD_INT32_LEN +
			sizeof(MSS_STREAM_INDEX_FOOTER);

		result_size += segment_durations[media_type].segment_count * (sizeof(MSS_CHUNK_TAG) + VOD_INT32_LEN + VOD_INT64_LEN);
	}

	mss_packager_remove_redundant_tracks(conf->duplicate_bitrate_threshold,	media_set);

	cur_track = media_set->filtered_tracks;
	last_track = cur_track + media_set->total_track_count;
	for (; cur_track < last_track; cur_track++)
	{
		switch (cur_track->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			result_size +=
				sizeof(MSS_VIDEO_QUALITY_LEVEL_HEADER) - 1 + 4 * VOD_INT32_LEN + cur_track->media_info.extra_data_size * 2 +
				sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1;
			break;

		case MEDIA_TYPE_AUDIO:
			result_size +=
				sizeof(MSS_AUDIO_QUALITY_LEVEL_HEADER) - 1 + 6 * VOD_INT32_LEN + cur_track->media_info.extra_data_size * 2 +
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

	duration_100ns = rescale_time(media_set->total_duration, 1000, MSS_TIMESCALE);
	p = vod_sprintf(result->data, MSS_MANIFEST_HEADER, duration_100ns);

	if (media_set->track_count[MEDIA_TYPE_VIDEO] != 0)
	{
		p = vod_sprintf(p, 
			MSS_STREAM_INDEX_HEADER, 
			MSS_STREAM_TYPE_VIDEO,
			media_set->track_count[MEDIA_TYPE_VIDEO],
			segment_durations[MEDIA_TYPE_VIDEO].segment_count,
			MSS_STREAM_TYPE_VIDEO);

		stream_index = 0;
		for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
		{
			last_track = cur_sequence->filtered_clips[0].last_track;
			for (cur_track = cur_sequence->filtered_clips[0].first_track; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type != MEDIA_TYPE_VIDEO)
				{
					continue;
				}

				bitrate = cur_track->media_info.bitrate;
				bitrate = mss_encode_indexes(bitrate, cur_sequence->index, cur_track->index);
				p = vod_sprintf(p, MSS_VIDEO_QUALITY_LEVEL_HEADER,
					stream_index++,
					bitrate,
					(uint32_t)cur_track->media_info.u.video.width,
					(uint32_t)cur_track->media_info.u.video.height);

				p = mss_append_hex_string(p, cur_track->media_info.extra_data, cur_track->media_info.extra_data_size);

				p = vod_copy(p, MSS_QUALITY_LEVEL_FOOTER, sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1);
			}
		}

		p = mss_write_manifest_chunks(p, &segment_durations[MEDIA_TYPE_VIDEO]);

		p = vod_copy(p, MSS_STREAM_INDEX_FOOTER, sizeof(MSS_STREAM_INDEX_FOOTER) - 1);
	}

	if (media_set->track_count[MEDIA_TYPE_AUDIO] != 0)
	{
		p = vod_sprintf(p, MSS_STREAM_INDEX_HEADER, 
			MSS_STREAM_TYPE_AUDIO,
			media_set->track_count[MEDIA_TYPE_AUDIO],
			segment_durations[MEDIA_TYPE_AUDIO].segment_count,
			MSS_STREAM_TYPE_AUDIO);

		stream_index = 0;
		for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
		{
			last_track = cur_sequence->filtered_clips[0].last_track;
			for (cur_track = cur_sequence->filtered_clips[0].first_track; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type != MEDIA_TYPE_AUDIO)
				{
					continue;
				}

				bitrate = cur_track->media_info.bitrate;
				bitrate = mss_encode_indexes(bitrate, cur_sequence->index, cur_track->index);
				p = vod_sprintf(p, MSS_AUDIO_QUALITY_LEVEL_HEADER,
					stream_index++,
					bitrate,
					cur_track->media_info.u.audio.sample_rate,
					(uint32_t)cur_track->media_info.u.audio.channels,
					(uint32_t)cur_track->media_info.u.audio.bits_per_sample,
					(uint32_t)cur_track->media_info.u.audio.packet_size);

				p = mss_append_hex_string(p, cur_track->media_info.extra_data, cur_track->media_info.extra_data_size);

				p = vod_copy(p, MSS_QUALITY_LEVEL_FOOTER, sizeof(MSS_QUALITY_LEVEL_FOOTER) - 1);
			}
		}

		p = mss_write_manifest_chunks(p, &segment_durations[MEDIA_TYPE_AUDIO]);

		p = vod_copy(p, MSS_STREAM_INDEX_FOOTER, sizeof(MSS_STREAM_INDEX_FOOTER) - 1);
	}

	if (write_extra_tags != NULL)
	{
		p = write_extra_tags(extra_tags_writer_context, p, media_set);
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
mss_write_uuid_tfxd_atom(u_char* p, media_sequence_t* sequence)
{
	media_clip_filtered_t* cur_clip;
	media_track_t* track;
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(uuid_tfxd_atom_t);
	uint64_t timestamp;
	uint64_t duration;

	cur_clip = sequence->filtered_clips;
	track = cur_clip->first_track;
	timestamp = rescale_time(track->first_frame_time_offset + track->clip_sequence_offset, track->media_info.timescale, MSS_TIMESCALE);
	duration = rescale_time(track->total_frames_duration, track->media_info.timescale, MSS_TIMESCALE);
	cur_clip++;

	for (; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		track = cur_clip->first_track;
		duration += rescale_time(track->total_frames_duration, track->media_info.timescale, MSS_TIMESCALE);
	}

	write_atom_header(p, atom_size, 'u', 'u', 'i', 'd');
	p = vod_copy(p, tfxd_uuid, sizeof(tfxd_uuid));
	write_dword(p, 0x01000000);		// version / flags
	write_qword(p, timestamp);
	write_qword(p, duration);
	return p;
}

vod_status_t
mss_packager_build_fragment_header(
	request_context_t* request_context,
	media_sequence_t* sequence,
	uint32_t segment_index,
	size_t extra_traf_atoms_size,
	write_extra_traf_atoms_callback_t write_extra_traf_atoms_callback,
	void* write_extra_traf_atoms_context,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size)
{
	media_clip_filtered_t* cur_clip;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	media_track_t* first_track = sequence->filtered_clips[0].first_track;
	media_track_t* track;
	uint32_t media_type = sequence->media_type;
	size_t mdat_atom_size;
	size_t trun_atom_size;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t result_size;
	u_char* p;

	// calculate sizes
	mdat_atom_size = ATOM_HEADER_SIZE + sequence->total_frame_size;
	trun_atom_size = mp4_builder_get_trun_atom_size(media_type, sequence->total_frame_count);

	traf_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(tfhd_atom_t) +
		trun_atom_size +
		ATOM_HEADER_SIZE + sizeof(uuid_tfxd_atom_t) + 
		extra_traf_atoms_size;

	moof_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t)+
		traf_atom_size;

	result_size =
		moof_atom_size +
		ATOM_HEADER_SIZE;		// mdat

	*total_fragment_size = result_size + sequence->total_frame_size;

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
	switch (media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = mss_write_tfhd_atom(p, first_track->media_info.track_id, 0x01010000);
		break;

	case MEDIA_TYPE_AUDIO:
		p = mss_write_tfhd_atom(p, first_track->media_info.track_id, 0x02000000);
		break;
	}

	// moof.traf.trun
	for (cur_clip = sequence->filtered_clips; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		track = cur_clip->first_track;
		cur_frame = track->first_frame;
		last_frame = track->last_frame;
		for (; cur_frame < last_frame; cur_frame++)
		{
			cur_frame->duration = rescale_time(cur_frame->duration, track->media_info.timescale, MSS_TIMESCALE);
			cur_frame->pts_delay = rescale_time(cur_frame->pts_delay, track->media_info.timescale, MSS_TIMESCALE);
		}
	}

	p = mp4_builder_write_trun_atom(
		p,
		sequence,
		moof_atom_size + ATOM_HEADER_SIZE);

	p = mss_write_uuid_tfxd_atom(p, sequence);

	// moof.traf.xxx
	if (write_extra_traf_atoms_callback != NULL)
	{
		p = write_extra_traf_atoms_callback(write_extra_traf_atoms_context, p, moof_atom_size + ATOM_HEADER_SIZE);
	}

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
