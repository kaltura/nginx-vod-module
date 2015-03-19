#include "dash_packager.h"

// macros
#define vod_copy_atom(p, raw_atom) vod_copy(p, (raw_atom).ptr, (raw_atom).size)

// constants
#define VOD_DASH_MANIFEST_HEADER												\
    "<?xml version=\"1.0\"?>\n"													\
    "<MPD\n"																	\
	"    xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"				\
	"    xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n"								\
	"    xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 http://standards.iso.org/ittf/PubliclyAvailableStandards/MPEG-DASH_schema_files/DASH-MPD.xsd\"\n"	\
	"    type=\"static\"\n"														\
	"    mediaPresentationDuration=\"PT%uD.%uDS\"\n"							\
	"    minBufferTime=\"PT%uDS\"\n"											\
	"    profiles=\"urn:mpeg:dash:profile:isoff-main:2011\">\n"					\
	"    <Period>\n"

#define VOD_DASH_MANIFEST_VIDEO_HEADER											\
    "    <AdaptationSet\n"														\
    "        id=\"1\"\n"														\
    "        segmentAlignment=\"true\"\n"										\
    "        maxWidth=\"%uD\"\n"												\
    "        maxHeight=\"%uD\"\n"												\
    "        maxFrameRate=\"%uD.%03uD\">\n"

#define VOD_DASH_MANIFEST_VIDEO_PREFIX											\
	"      <Representation\n"													\
    "          id=\"f%uD-v%uD\"\n"												\
    "          mimeType=\"video/mp4\"\n"										\
    "          codecs=\"%V\"\n"													\
    "          width=\"%uD\"\n"													\
    "          height=\"%uD\"\n"												\
    "          frameRate=\"%uD.%03uD\"\n"										\
    "          sar=\"1:1\"\n"													\
    "          startWithSAP=\"1\"\n"											\
	"          bandwidth=\"%uD\">\n"

#define VOD_DASH_MANIFEST_VIDEO_SUFFIX											\
	"      </Representation>\n"

#define VOD_DASH_MANIFEST_VIDEO_FOOTER											\
    "    </AdaptationSet>\n"

#define VOD_DASH_MANIFEST_AUDIO_HEADER											\
    "    <AdaptationSet\n"														\
    "        id=\"2\"\n"														\
    "        segmentAlignment=\"true\">\n"										\
    "      <AudioChannelConfiguration\n"										\
    "          schemeIdUri=\"urn:mpeg:dash:"									\
                                "23003:3:audio_channel_configuration:2011\"\n"	\
    "          value=\"1\"/>\n"

#define VOD_DASH_MANIFEST_AUDIO_PREFIX											\
	"      <Representation\n"													\
	"          id=\"f%uD-a%uD\"\n"												\
    "          mimeType=\"audio/mp4\"\n"										\
    "          codecs=\"%V\"\n"													\
    "          audioSamplingRate=\"%uD\"\n"										\
    "          startWithSAP=\"1\"\n"											\
    "          bandwidth=\"%uD\">\n"

#define VOD_DASH_MANIFEST_AUDIO_SUFFIX											\
	"      </Representation>\n"

#define VOD_DASH_MANIFEST_AUDIO_FOOTER											\
    "    </AdaptationSet>\n"

#define VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FIXED								\
	"        <SegmentTemplate\n"												\
	"            timescale=\"1000\"\n"											\
	"            media=\"%V%V-$Number$-$RepresentationID$.m4s\"\n"				\
	"            initialization=\"%V%V-$RepresentationID$.mp4\"\n"				\
	"            duration=\"%ui\"\n"											\
	"            startNumber=\"1\">\n"											\
	"        </SegmentTemplate>\n"

#define VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_HEADER								\
	"        <SegmentTemplate\n"												\
	"            timescale=\"1000\"\n"											\
	"            media=\"%V%V-$Number$-$RepresentationID$.m4s\"\n"				\
	"            initialization=\"%V%V-$RepresentationID$.mp4\"\n"				\
	"            startNumber=\"1\">\n"											\
	"            <SegmentTimeline>\n"

#define VOD_DASH_MANIFEST_SEGMENT_REPEAT                                        \
	"                <S d=\"%uD\" r=\"%uD\"/>\n"

#define VOD_DASH_MANIFEST_SEGMENT                                               \
	"                <S d=\"%uD\"/>\n"

#define VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER								\
	"            </SegmentTimeline>\n"											\
	"        </SegmentTemplate>\n"

#define VOD_DASH_MANIFEST_FOOTER												\
    "  </Period>\n"																\
    "</MPD>\n"

// init mp4 atoms
typedef struct {
	size_t track_stbl_size;
	size_t track_minf_size;
	size_t track_mdia_size;
	size_t track_trak_size;
} track_sizes_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char track_id[4];
	u_char default_sample_description_index[4];
	u_char default_sample_duration[4];
	u_char default_sample_size[4];
	u_char default_sample_flags[4];
} trex_atom_t;

// fragment atoms
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char reference_id[4];
	u_char timescale[4];
	u_char earliest_pres_time[4];
	u_char first_offset[4];
	u_char reserved[2];
	u_char reference_count[2];
	u_char reference_size[4];			// Note: from this point forward, assuming reference_count == 1
	u_char subsegment_duration[4];
	u_char sap_type[1];
	u_char sap_delta_time[3];
} sidx_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char reference_id[4];
	u_char timescale[4];
	u_char earliest_pres_time[8];
	u_char first_offset[8];
	u_char reserved[2];
	u_char reference_count[2];
	u_char reference_size[4];			// Note: from this point forward, assuming reference_count == 1
	u_char subsegment_duration[4];
	u_char sap_type[1];
	u_char sap_delta_time[3];
} sidx64_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char track_id[4];
} tfhd_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char earliest_pres_time[4];
} tfdt_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char earliest_pres_time[8];
} tfdt64_atom_t;

// fixed init mp4 atoms

static const u_char ftyp_atom[] = {
	0x00, 0x00, 0x00, 0x18,		// atom size
	0x66, 0x74, 0x79, 0x70,		// ftyp
	0x69, 0x73, 0x6f, 0x6d,		// major brand
	0x00, 0x00, 0x00, 0x01,		// minor version
	0x69, 0x73, 0x6f, 0x6d,		// compatible brand
	0x61, 0x76, 0x63, 0x31,		// compatible brand
};

static const u_char vmhd_atom[] = {
	0x00, 0x00, 0x00, 0x14,		// atom size
	0x76, 0x6d, 0x68, 0x64,		// vmhd
	0x00, 0x00, 0x00, 0x01,		// version & flags
	0x00, 0x00, 0x00, 0x00,		// reserved
	0x00, 0x00, 0x00, 0x00,		// reserved
};

static const u_char smhd_atom[] = {
	0x00, 0x00, 0x00, 0x10,		// atom size
	0x73, 0x6d, 0x68, 0x64,		// smhd
	0x00, 0x00, 0x00, 0x00,		// version & flags
	0x00, 0x00, 0x00, 0x00,		// reserved
};

static const u_char fixed_stbl_atoms[] = {
	0x00, 0x00, 0x00, 0x10,		// atom size
	0x73, 0x74, 0x74, 0x73,		// stts
	0x00, 0x00, 0x00, 0x00,		// version
	0x00, 0x00, 0x00, 0x00,		// entry count
	0x00, 0x00, 0x00, 0x10,		// atom size
	0x73, 0x74, 0x73, 0x63,		// stsc
	0x00, 0x00, 0x00, 0x00,		// version
	0x00, 0x00, 0x00, 0x00,		// entry count
	0x00, 0x00, 0x00, 0x14,		// atom size
	0x73, 0x74, 0x73, 0x7a,		// stsz
	0x00, 0x00, 0x00, 0x00,		// version
	0x00, 0x00, 0x00, 0x00,		// uniform size
	0x00, 0x00, 0x00, 0x00,		// entry count
	0x00, 0x00, 0x00, 0x10, 	// atom size
	0x73, 0x74, 0x63, 0x6f,		// stco
	0x00, 0x00, 0x00, 0x00,		// version
	0x00, 0x00, 0x00, 0x00,		// entry count
};

// fixed fragment atoms

static const u_char styp_atom[] = {
	0x00, 0x00, 0x00, 0x1c,		// atom size
	0x73, 0x74, 0x79, 0x70,		// styp
	0x69, 0x73, 0x6f, 0x36,		// major brand
	0x00, 0x00, 0x00, 0x01,		// minor version
	0x69, 0x73, 0x6f, 0x6d,		// compatible brand
	0x69, 0x73, 0x6f, 0x36,		// compatible brand
	0x64, 0x61, 0x73, 0x68,		// compatible brand
};

// mpd writing code

bool_t
dash_packager_compare_streams(void* context, const media_info_t* mi1, const media_info_t* mi2)
{
	uintptr_t bitrate_threshold = *(uintptr_t*)context;

	if (mi1->bitrate == 0 ||
		mi2->bitrate == 0 ||
		mi1->bitrate + bitrate_threshold <= mi2->bitrate ||
		mi2->bitrate + bitrate_threshold <= mi1->bitrate)
	{
		return FALSE;
	}

	if (mi1->codec_name.len != mi2->codec_name.len ||
		vod_memcmp(mi1->codec_name.data, mi2->codec_name.data, mi2->codec_name.len) != 0)
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

static u_char* 
dash_packager_write_segment_template(
	u_char* p, 
	dash_manifest_config_t* conf,
	segmenter_conf_t* segmenter_conf,
	vod_str_t* base_url,
	segment_durations_t* segment_durations)
{
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	uint32_t duration;

	if (!conf->segment_timeline)
	{
		return vod_sprintf(p,
			VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FIXED,
			base_url,
			&conf->fragment_file_name_prefix,
			base_url,
			&conf->init_file_name_prefix,
			segmenter_conf->segment_duration);
	}

	p = vod_sprintf(p,
		VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_HEADER,
		base_url,
		&conf->fragment_file_name_prefix,
		base_url,
		&conf->init_file_name_prefix);

	for (cur_item = segment_durations->items; cur_item < last_item; cur_item++)
	{
		duration = (uint32_t)rescale_time(cur_item->duration, segment_durations->timescale, 1000);

		if (cur_item->repeat_count == 1)
		{
			p = vod_sprintf(p, VOD_DASH_MANIFEST_SEGMENT, duration);
		}
		else if (cur_item->repeat_count > 1)
		{
			p = vod_sprintf(p, VOD_DASH_MANIFEST_SEGMENT_REPEAT, duration, cur_item->repeat_count - 1);
		}
	}

	p = vod_copy(p, VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER, sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER) - 1);

	return p;
}

vod_status_t 
dash_packager_build_mpd(
	request_context_t* request_context, 
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	segmenter_conf_t* segmenter_conf,
	mpeg_metadata_t* mpeg_metadata, 
	size_t representation_tags_size,
	write_tags_callback_t write_representation_tags,
	void* representation_tags_writer_context,
	vod_str_t* result)
{
	mpeg_stream_metadata_t* cur_stream;
	segment_durations_t segment_durations[MEDIA_TYPE_COUNT];
	size_t result_size;
	size_t urls_length;
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	uint32_t max_framerate_duration = 0;
	uint32_t max_framerate_timescale = 0;
	uint32_t media_type;
	vod_status_t rc;
	u_char* p;

	// calculate the total size
	urls_length = 2 * base_url->len + conf->init_file_name_prefix.len + conf->fragment_file_name_prefix.len;
	result_size =
		sizeof(VOD_DASH_MANIFEST_HEADER) - 1 + 3 * VOD_INT32_LEN +
			sizeof(VOD_DASH_MANIFEST_VIDEO_HEADER) - 1 + 4 * VOD_INT32_LEN + 
				mpeg_metadata->stream_count[MEDIA_TYPE_VIDEO] * (
					sizeof(VOD_DASH_MANIFEST_VIDEO_PREFIX) - 1 + 7 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
					sizeof(VOD_DASH_MANIFEST_VIDEO_SUFFIX) - 1) +
			sizeof(VOD_DASH_MANIFEST_VIDEO_FOOTER) - 1 +
			sizeof(VOD_DASH_MANIFEST_AUDIO_HEADER) - 1 + 
				mpeg_metadata->stream_count[MEDIA_TYPE_AUDIO] * (
					sizeof(VOD_DASH_MANIFEST_AUDIO_PREFIX) - 1 + 4 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
					sizeof(VOD_DASH_MANIFEST_AUDIO_SUFFIX) - 1) +
			sizeof(VOD_DASH_MANIFEST_AUDIO_FOOTER) - 1 +
		sizeof(VOD_DASH_MANIFEST_FOOTER) +
		representation_tags_size;

	// get the segment count
	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		if (mpeg_metadata->longest_stream[media_type] == NULL)
		{
			continue;
		}

		rc = segmenter_conf->get_segment_durations(
			request_context,
			segmenter_conf,
			&mpeg_metadata->longest_stream[media_type],
			1,
			&segment_durations[media_type]);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (conf->segment_timeline)
		{
			result_size += 
				sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_HEADER) - 1 + urls_length +
					(sizeof(VOD_DASH_MANIFEST_SEGMENT_REPEAT) - 1 + 2 * VOD_INT32_LEN) * segment_durations[media_type].item_count +
				sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER) - 1;
		}
		else
		{
			result_size += sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FIXED) - 1 + urls_length + VOD_INT64_LEN;
		}
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dash_packager_build_mpd: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	
	// print the manifest header
	p = vod_sprintf(result->data, 
		VOD_DASH_MANIFEST_HEADER,
		(uint32_t)(mpeg_metadata->duration_millis / 1000),
		(uint32_t)(mpeg_metadata->duration_millis % 1000),
		(uint32_t)(segmenter_conf->max_segment_duration / 1000));

	// video adaptation set
	if (mpeg_metadata->stream_count[MEDIA_TYPE_VIDEO])
	{
		// get the max width, height and frame rate
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_VIDEO)
			{
				continue;
			}

			if (cur_stream->media_info.u.video.width > max_width)
			{
				max_width = cur_stream->media_info.u.video.width;
			}

			if (cur_stream->media_info.u.video.height > max_height)
			{
				max_height = cur_stream->media_info.u.video.height;
			}

			if (max_framerate_duration == 0 || 
				cur_stream->media_info.timescale * max_framerate_duration >
				max_framerate_timescale * cur_stream->media_info.min_frame_duration)
			{
				max_framerate_duration = cur_stream->media_info.min_frame_duration;
				max_framerate_timescale = cur_stream->media_info.timescale;
			}
		}

		// print the header
		p = vod_sprintf(p, 
			VOD_DASH_MANIFEST_VIDEO_HEADER,
			max_width,
			max_height,
			(uint32_t)max_framerate_timescale / max_framerate_duration,
			(uint32_t)(((uint64_t)max_framerate_timescale * 1000) / max_framerate_duration % 1000));

		// print the segment template
		p = dash_packager_write_segment_template(
			p,
			conf,
			segmenter_conf,
			base_url,
			&segment_durations[MEDIA_TYPE_VIDEO]);
			
		// print the representations
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_VIDEO)
			{
				continue;
			}

			p = vod_sprintf(p, 
				VOD_DASH_MANIFEST_VIDEO_PREFIX,
				cur_stream->file_info.file_index + 1,
				cur_stream->track_index + 1,
				&cur_stream->media_info.codec_name,
				(uint32_t)cur_stream->media_info.u.video.width,
				(uint32_t)cur_stream->media_info.u.video.height,
				(uint32_t)(cur_stream->media_info.timescale / cur_stream->media_info.min_frame_duration),
				(uint32_t)(((uint64_t)cur_stream->media_info.timescale * 1000) / cur_stream->media_info.min_frame_duration % 1000),
				cur_stream->media_info.bitrate
				);

			// write any additional tags
			if (write_representation_tags != NULL)
			{
				p = write_representation_tags(representation_tags_writer_context, p, cur_stream);
			}

			p = vod_copy(p, VOD_DASH_MANIFEST_VIDEO_SUFFIX, sizeof(VOD_DASH_MANIFEST_VIDEO_SUFFIX) - 1);
		}

		// print the footer
		p = vod_copy(p, VOD_DASH_MANIFEST_VIDEO_FOOTER, sizeof(VOD_DASH_MANIFEST_VIDEO_FOOTER) - 1);
	}

	// audio adaptation set
	if (mpeg_metadata->stream_count[MEDIA_TYPE_AUDIO])
	{
		// print the header
		p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_HEADER, sizeof(VOD_DASH_MANIFEST_AUDIO_HEADER) - 1);

		// print the segment template
		p = dash_packager_write_segment_template(
			p,
			conf,
			segmenter_conf,
			base_url,
			&segment_durations[MEDIA_TYPE_AUDIO]);

		// print the representations
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_AUDIO)
			{
				continue;
			}

			p = vod_sprintf(p,
				VOD_DASH_MANIFEST_AUDIO_PREFIX,
				cur_stream->file_info.file_index + 1,
				cur_stream->track_index + 1,
				&cur_stream->media_info.codec_name,
				cur_stream->media_info.u.audio.sample_rate,
				cur_stream->media_info.bitrate);

			// write any additional tags
			if (write_representation_tags != NULL)
			{
				p = write_representation_tags(representation_tags_writer_context, p, cur_stream);
			}

			p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_SUFFIX, sizeof(VOD_DASH_MANIFEST_AUDIO_SUFFIX) - 1);
		}

		// print the footer
		p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_FOOTER, sizeof(VOD_DASH_MANIFEST_AUDIO_FOOTER) - 1);
	}

	p = vod_copy(p, VOD_DASH_MANIFEST_FOOTER, sizeof(VOD_DASH_MANIFEST_FOOTER) - 1);

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dash_packager_build_mpd: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}


// init mp4 writing code

static void
dash_packager_get_track_sizes(mpeg_stream_metadata_t* cur_stream, size_t stsd_size, track_sizes_t* result)
{
	result->track_stbl_size = ATOM_HEADER_SIZE + stsd_size + sizeof(fixed_stbl_atoms);
	result->track_minf_size = ATOM_HEADER_SIZE + cur_stream->raw_atoms[RTA_DINF].size + result->track_stbl_size;
	switch (cur_stream->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		result->track_minf_size += sizeof(vmhd_atom);
		break;
	case MEDIA_TYPE_AUDIO:
		result->track_minf_size += sizeof(smhd_atom);
		break;
	}
	result->track_mdia_size = ATOM_HEADER_SIZE + cur_stream->raw_atoms[RTA_MDHD].size + cur_stream->raw_atoms[RTA_HDLR].size + result->track_minf_size;
	result->track_trak_size = ATOM_HEADER_SIZE + cur_stream->raw_atoms[RTA_TKHD].size + result->track_mdia_size;
}

static u_char*
dash_packager_write_trex_atom(u_char* p, uint32_t track_id)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(trex_atom_t);

	write_atom_header(p, atom_size, 't', 'r', 'e', 'x');
	write_dword(p, 0);			// version + flags
	write_dword(p, track_id);	// track id
	write_dword(p, 1);			// default sample description index
	write_dword(p, 0);			// default sample duration
	write_dword(p, 0);			// default sample size
	write_dword(p, 0);			// default sample size
	return p;
}

vod_status_t
dash_packager_build_init_mp4(
	request_context_t* request_context, 
	mpeg_metadata_t* mpeg_metadata, 
	bool_t size_only,
	atom_writer_t* extra_moov_atoms_writer,
	atom_writer_t* stsd_atom_writer,
	vod_str_t* result)
{
	track_sizes_t track_sizes;
	size_t moov_atom_size;
	size_t mvex_atom_size;
	size_t result_size;
	size_t stsd_size;
	u_char* p;

	// calc moov atom size
	if (stsd_atom_writer != NULL)
	{
		stsd_size = stsd_atom_writer->atom_size;
	}
	else
	{
		stsd_size = mpeg_metadata->first_stream->raw_atoms[RTA_STSD].size;
	}

	dash_packager_get_track_sizes(mpeg_metadata->first_stream, stsd_size, &track_sizes);

	mvex_atom_size = ATOM_HEADER_SIZE + ATOM_HEADER_SIZE + sizeof(trex_atom_t);

	moov_atom_size = ATOM_HEADER_SIZE +
		mpeg_metadata->mvhd_atom.size +
		track_sizes.track_trak_size +
		mvex_atom_size;

	if (extra_moov_atoms_writer != NULL)
	{
		moov_atom_size += extra_moov_atoms_writer->atom_size;
	}

	result_size = sizeof(ftyp_atom) + moov_atom_size;

	// head request optimization
	if (size_only)
	{
		result->len = result_size;
		return VOD_OK;
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dash_packager_build_init_mp4: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// ftyp
	p = vod_copy(result->data, ftyp_atom, sizeof(ftyp_atom));

	// moov
	write_atom_header(p, moov_atom_size, 'm', 'o', 'o', 'v');

	// moov.mvhd
	p = vod_copy_atom(p, mpeg_metadata->mvhd_atom);

	// moov.mvex
	write_atom_header(p, mvex_atom_size, 'm', 'v', 'e', 'x');

	// moov.mvex.trex
	p = dash_packager_write_trex_atom(p, mpeg_metadata->first_stream->media_info.track_id);

	// moov.trak
	write_atom_header(p, track_sizes.track_trak_size, 't', 'r', 'a', 'k');
	p = vod_copy_atom(p, mpeg_metadata->first_stream->raw_atoms[RTA_TKHD]);

	// moov.trak.mdia
	write_atom_header(p, track_sizes.track_mdia_size, 'm', 'd', 'i', 'a');
	p = vod_copy_atom(p, mpeg_metadata->first_stream->raw_atoms[RTA_MDHD]);
	p = vod_copy_atom(p, mpeg_metadata->first_stream->raw_atoms[RTA_HDLR]);

	// moov.trak.minf
	write_atom_header(p, track_sizes.track_minf_size, 'm', 'i', 'n', 'f');
	switch (mpeg_metadata->first_stream->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = vod_copy(p, vmhd_atom, sizeof(vmhd_atom));
		break;
	case MEDIA_TYPE_AUDIO:
		p = vod_copy(p, smhd_atom, sizeof(smhd_atom));
		break;
	}
	p = vod_copy_atom(p, mpeg_metadata->first_stream->raw_atoms[RTA_DINF]);

	// moov.trak.minf.stbl
	write_atom_header(p, track_sizes.track_stbl_size, 's', 't', 'b', 'l');
	if (stsd_atom_writer != NULL)
	{
		p = stsd_atom_writer->write(stsd_atom_writer->context, p);
	}
	else
	{
		p = vod_copy_atom(p, mpeg_metadata->first_stream->raw_atoms[RTA_STSD]);
	}
	p = vod_copy(p, fixed_stbl_atoms, sizeof(fixed_stbl_atoms));

	// moov.xxx
	if (extra_moov_atoms_writer != NULL)
	{
		p = extra_moov_atoms_writer->write(extra_moov_atoms_writer->context, p);
	}

	result->len = p - result->data;

	if (result->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dash_packager_build_init_mp4: result length %uz different than allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

// fragment writing code

static uint64_t 
dash_packager_get_earliest_pres_time(mpeg_stream_metadata_t* stream_metadata)
{
	uint64_t result = stream_metadata->first_frame_time_offset;

	if (stream_metadata->frame_count > 0)
	{
		result += stream_metadata->frames[0].pts_delay;
	}
	return result;
}

static u_char*
dash_packager_write_sidx_atom(
	u_char* p,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t earliest_pres_time,
	uint32_t reference_size)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(sidx_atom_t);

	write_atom_header(p, atom_size, 's', 'i', 'd', 'x');
	write_dword(p, 0);					// version + flags
	write_dword(p, 1);					// reference id
	write_dword(p, stream_metadata->media_info.timescale);				// timescale
	write_dword(p, earliest_pres_time);	// earliest presentation time
	write_dword(p, 0);					// first offset
	write_dword(p, 1);					// reserved + reference count
	write_dword(p, reference_size);		// referenced size
	write_dword(p, stream_metadata->total_frames_duration);		// subsegment duration
	write_dword(p, 0x90000000);			// starts with SAP / SAP type
	return p;
}

static u_char*
dash_packager_write_sidx64_atom(
	u_char* p,
	mpeg_stream_metadata_t* stream_metadata,
	uint64_t earliest_pres_time,
	uint32_t reference_size)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(sidx64_atom_t);

	write_atom_header(p, atom_size, 's', 'i', 'd', 'x');
	write_dword(p, 0x01000000);			// version + flags
	write_dword(p, 1);					// reference id
	write_dword(p, stream_metadata->media_info.timescale);				// timescale
	write_qword(p, earliest_pres_time);	// earliest presentation time
	write_qword(p, 0LL);					// first offset
	write_dword(p, 1);					// reserved + reference count
	write_dword(p, reference_size);		// referenced size
	write_dword(p, stream_metadata->total_frames_duration);		// subsegment duration
	write_dword(p, 0x90000000);			// starts with SAP / SAP type
	return p;
}

static u_char*
dash_packager_write_tfhd_atom(u_char* p, uint32_t track_id, uint32_t sample_description_index)
{
	size_t atom_size;
	uint32_t flags;

	flags = 0x020000;				// default-base-is-moof
	atom_size = ATOM_HEADER_SIZE + sizeof(tfhd_atom_t);
	if (sample_description_index > 0)
	{
		flags |= 0x02;				// sample-description-index-present
		atom_size += sizeof(uint32_t);
	}

	write_atom_header(p, atom_size, 't', 'f', 'h', 'd');
	write_dword(p, flags);			// flags
	write_dword(p, track_id);		// track id
	if (sample_description_index > 0)
	{
		write_dword(p, sample_description_index);
	}
	return p;
}

static u_char*
dash_packager_write_tfdt_atom(u_char* p, uint32_t earliest_pres_time)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfdt_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'd', 't');
	write_dword(p, 0);
	write_dword(p, earliest_pres_time);
	return p;
}

static u_char*
dash_packager_write_tfdt64_atom(u_char* p, uint64_t earliest_pres_time)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfdt64_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'd', 't');
	write_dword(p, 0x01000000);			// version = 1
	write_qword(p, earliest_pres_time);
	return p;
}

vod_status_t
dash_packager_build_fragment_header(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	uint32_t sample_description_index,
	size_t extra_traf_atoms_size,
	write_extra_traf_atoms_callback_t write_extra_traf_atoms_callback,
	void* write_extra_traf_atoms_context,
	atom_writer_t* mdat_prefix_writer,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size)
{
	uint64_t earliest_pres_time = dash_packager_get_earliest_pres_time(stream_metadata);
	size_t first_frame_offset;
	size_t mdat_atom_size;
	size_t trun_atom_size;
	size_t tfhd_atom_size;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t result_size;
	u_char* p;

	// calculate sizes
	mdat_atom_size = ATOM_HEADER_SIZE + stream_metadata->total_frames_size;
	if (mdat_prefix_writer != NULL)
	{
		mdat_atom_size += mdat_prefix_writer->atom_size;
	}
	trun_atom_size = mp4_builder_get_trun_atom_size(stream_metadata->media_info.media_type, stream_metadata->frame_count);

	tfhd_atom_size = ATOM_HEADER_SIZE + sizeof(tfhd_atom_t);
	if (sample_description_index > 0)
	{
		tfhd_atom_size += sizeof(uint32_t);
	}

	traf_atom_size =
		ATOM_HEADER_SIZE +
		tfhd_atom_size +
		ATOM_HEADER_SIZE + (earliest_pres_time > UINT_MAX ? sizeof(tfdt64_atom_t) : sizeof(tfdt_atom_t)) +
		trun_atom_size + 
		extra_traf_atoms_size;

	moof_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t)+
		traf_atom_size;

	result_size =
		sizeof(styp_atom)+
		ATOM_HEADER_SIZE + (earliest_pres_time > UINT_MAX ? sizeof(sidx64_atom_t) : sizeof(sidx_atom_t)) + 
		moof_atom_size +
		mdat_atom_size - stream_metadata->total_frames_size;		// mdat

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
			"dash_packager_build_fragment_header: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// styp
	p = vod_copy(result->data, styp_atom, sizeof(styp_atom));

	// sidx
	if (earliest_pres_time > UINT_MAX)
	{
		p = dash_packager_write_sidx64_atom(p, stream_metadata, earliest_pres_time, moof_atom_size + mdat_atom_size);
	}
	else
	{
		p = dash_packager_write_sidx_atom(p, stream_metadata, (uint32_t)earliest_pres_time, moof_atom_size + mdat_atom_size);
	}

	// moof
	write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

	// moof.mfhd
	p = mp4_builder_write_mfhd_atom(p, segment_index);

	// moof.traf
	write_atom_header(p, traf_atom_size, 't', 'r', 'a', 'f');

	// moof.traf.tfhd
	p = dash_packager_write_tfhd_atom(p, stream_metadata->media_info.track_id, sample_description_index);

	// moof.traf.tfdt
	if (earliest_pres_time > UINT_MAX)
	{
		p = dash_packager_write_tfdt64_atom(p, earliest_pres_time);
	}
	else
	{
		p = dash_packager_write_tfdt_atom(p, (uint32_t)earliest_pres_time);
	}

	// moof.traf.trun
	first_frame_offset = moof_atom_size + ATOM_HEADER_SIZE;
	if (mdat_prefix_writer != NULL)
	{
		first_frame_offset += mdat_prefix_writer->atom_size;
	}

	p = mp4_builder_write_trun_atom(
		p, 
		stream_metadata->media_info.media_type, 
		stream_metadata->frames, 
		stream_metadata->frame_count, 
		first_frame_offset);

	// moof.traf.xxx
	if (write_extra_traf_atoms_callback != NULL)
	{
		p = write_extra_traf_atoms_callback(write_extra_traf_atoms_context, p, moof_atom_size + ATOM_HEADER_SIZE);
	}

	// mdat
	write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');

	// mdat prefix
	if (mdat_prefix_writer != NULL)
	{
		p = mdat_prefix_writer->write(mdat_prefix_writer->context, p);
	}

	result->len = p - result->data;

	if (result->len != result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dash_packager_build_fragment_header: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}
