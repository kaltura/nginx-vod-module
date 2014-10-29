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
    "        maxFrameRate=\"%uD.%03uD\">\n"										\
	"        <SegmentTemplate\n"												\
	"            presentationTimeOffset=\"0\"\n"								\
	"            timescale=\"1000\"\n"											\
	"            media=\"%V%V-$Number$-$RepresentationID$.m4s\"\n"				\
	"            initialization=\"%V%V-$RepresentationID$.mp4\"\n"				\
	"            duration=\"%uD\"\n"											\
	"            startNumber=\"1\">\n"											\
"        </SegmentTemplate>\n"

#define VOD_DASH_MANIFEST_VIDEO													\
	"      <Representation\n"													\
    "          id=\"f%uD-v%uD\"\n"												\
    "          mimeType=\"video/mp4\"\n"										\
    "          codecs=\"%V\"\n"													\
    "          width=\"%uD\"\n"													\
    "          height=\"%uD\"\n"												\
    "          frameRate=\"%uD.%03uD\"\n"										\
    "          sar=\"1:1\"\n"													\
    "          startWithSAP=\"1\"\n"											\
	"          bandwidth=\"%uD\">\n"											\
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
    "          value=\"1\"/>\n"													\
	"        <SegmentTemplate\n"												\
	"            presentationTimeOffset=\"0\"\n"								\
	"            timescale=\"1000\"\n"											\
	"            media=\"%V%V-$Number$-$RepresentationID$.m4s\"\n"				\
	"            initialization=\"%V%V-$RepresentationID$.mp4\"\n"				\
	"            duration=\"%uD\"\n"											\
	"            startNumber=\"1\">\n"											\
	"        </SegmentTemplate>\n"

#define VOD_DASH_MANIFEST_AUDIO													\
	"      <Representation\n"													\
	"          id=\"f%uD-a%uD\"\n"												\
    "          mimeType=\"audio/mp4\"\n"										\
    "          codecs=\"%V\"\n"													\
    "          audioSamplingRate=\"%uD\"\n"										\
    "          startWithSAP=\"1\"\n"											\
    "          bandwidth=\"%uD\">\n"											\
	"      </Representation>\n"

#define VOD_DASH_MANIFEST_AUDIO_FOOTER											\
    "    </AdaptationSet>\n"

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
	u_char track_id[4];
} tfhd_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char earliest_pres_time[4];
} tfdt_atom_t;

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

vod_status_t 
dash_packager_build_mpd(
	request_context_t* request_context, 
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	uint32_t segment_duration,
	mpeg_metadata_t* mpeg_metadata, 
	vod_str_t* result)
{
	mpeg_stream_metadata_t* cur_stream;
	size_t result_size;
	size_t urls_length;
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	uint32_t max_framerate_duration = 0;
	uint32_t max_framerate_timescale = 0;
	u_char* p;

	// calculate the total size
	urls_length = 2 * base_url->len + conf->init_file_name_prefix.len + conf->fragment_file_name_prefix.len;
	result_size =
		sizeof(VOD_DASH_MANIFEST_HEADER) - 1 + 3 * VOD_INT32_LEN +
			sizeof(VOD_DASH_MANIFEST_VIDEO_HEADER) - 1 + 5 * VOD_INT32_LEN + urls_length +
				mpeg_metadata->stream_count[MEDIA_TYPE_VIDEO] * (sizeof(VOD_DASH_MANIFEST_VIDEO) - 1 + 7 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE) + 
			sizeof(VOD_DASH_MANIFEST_VIDEO_FOOTER) - 1 +
			sizeof(VOD_DASH_MANIFEST_AUDIO_HEADER) - 1 + 1 * VOD_INT32_LEN + urls_length +
				mpeg_metadata->stream_count[MEDIA_TYPE_AUDIO] * (sizeof(VOD_DASH_MANIFEST_AUDIO) - 1 + 4 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE) +
			sizeof(VOD_DASH_MANIFEST_AUDIO_FOOTER) - 1 +
		sizeof(VOD_DASH_MANIFEST_FOOTER);

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dash_packager_build_mpd: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	
	// print the manifest header
	p = vod_sprintf(result->data, VOD_DASH_MANIFEST_HEADER,
		(uint32_t)(mpeg_metadata->duration_millis / 1000),
		(uint32_t)(mpeg_metadata->duration_millis % 1000),
		(uint32_t)(segment_duration / 1000));

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
		p = vod_sprintf(p, VOD_DASH_MANIFEST_VIDEO_HEADER,
			max_width,
			max_height,
			(uint32_t)max_framerate_timescale / max_framerate_duration,
			(uint32_t)(((uint64_t)max_framerate_timescale * 1000) / max_framerate_duration % 1000),
			base_url,
			&conf->fragment_file_name_prefix,
			base_url,
			&conf->init_file_name_prefix,
			segment_duration);
			
		// print the representations
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_VIDEO)
			{
				continue;
			}

			p = vod_sprintf(p, VOD_DASH_MANIFEST_VIDEO,
				cur_stream->file_info.file_index + 1,
				cur_stream->track_index + 1,
				&cur_stream->media_info.codec_name,
				(uint32_t)cur_stream->media_info.u.video.width,
				(uint32_t)cur_stream->media_info.u.video.height,
				(uint32_t)(cur_stream->media_info.timescale / cur_stream->media_info.min_frame_duration),
				(uint32_t)(((uint64_t)cur_stream->media_info.timescale * 1000) / cur_stream->media_info.min_frame_duration % 1000),
				cur_stream->media_info.bitrate
				);
		}

		// print the footer
		p = vod_copy(p, VOD_DASH_MANIFEST_VIDEO_FOOTER, sizeof(VOD_DASH_MANIFEST_VIDEO_FOOTER) - 1);
	}

	// audio adaptation set
	if (mpeg_metadata->stream_count[MEDIA_TYPE_AUDIO])
	{
		// print the header
		p = vod_sprintf(p,
			VOD_DASH_MANIFEST_AUDIO_HEADER,
			base_url, 
			&conf->fragment_file_name_prefix,
			base_url, 
			&conf->init_file_name_prefix,
			segment_duration);
		
		// print the representations
		for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
		{
			if (cur_stream->media_info.media_type != MEDIA_TYPE_AUDIO)
			{
				continue;
			}

			p = vod_sprintf(p,
				VOD_DASH_MANIFEST_AUDIO,
				cur_stream->file_info.file_index + 1,
				cur_stream->track_index + 1,
				&cur_stream->media_info.codec_name,
				cur_stream->media_info.u.audio.sample_rate,
				cur_stream->media_info.bitrate);
		}

		// print the footer
		p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_FOOTER, sizeof(VOD_DASH_MANIFEST_AUDIO_FOOTER) - 1);
	}

	p = vod_sprintf(p, VOD_DASH_MANIFEST_FOOTER);

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
dash_packager_get_track_sizes(mpeg_stream_metadata_t* cur_stream, track_sizes_t* result)
{
	result->track_stbl_size = ATOM_HEADER_SIZE + cur_stream->raw_atoms[RTA_STSD].size + sizeof(fixed_stbl_atoms);
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
	vod_str_t* result)
{
	mpeg_stream_metadata_t* cur_stream;
	track_sizes_t track_sizes;
	size_t moov_atom_size;
	size_t mvex_atom_size;
	size_t result_size;
	u_char* p;

	// calc moov atom size
	moov_atom_size = ATOM_HEADER_SIZE + mpeg_metadata->mvhd_atom.size;
	mvex_atom_size = ATOM_HEADER_SIZE;
	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		// trak
		dash_packager_get_track_sizes(cur_stream, &track_sizes);
		moov_atom_size += track_sizes.track_trak_size;

		// trex
		mvex_atom_size += ATOM_HEADER_SIZE + sizeof(trex_atom_t);
	}
	moov_atom_size += mvex_atom_size;

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
	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		// moov.mvex.trex
		p = dash_packager_write_trex_atom(p, cur_stream->media_info.track_id);
	}

	// tracks
	for (cur_stream = mpeg_metadata->first_stream; cur_stream < mpeg_metadata->last_stream; cur_stream++)
	{
		dash_packager_get_track_sizes(cur_stream, &track_sizes);

		// moov.trak
		write_atom_header(p, track_sizes.track_trak_size, 't', 'r', 'a', 'k');
		p = vod_copy_atom(p, cur_stream->raw_atoms[RTA_TKHD]);

		// moov.trak.mdia
		write_atom_header(p, track_sizes.track_mdia_size, 'm', 'd', 'i', 'a');
		p = vod_copy_atom(p, cur_stream->raw_atoms[RTA_MDHD]);
		p = vod_copy_atom(p, cur_stream->raw_atoms[RTA_HDLR]);

		// moov.trak.minf
		write_atom_header(p, track_sizes.track_minf_size, 'm', 'i', 'n', 'f');
		switch (cur_stream->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			p = vod_copy(p, vmhd_atom, sizeof(vmhd_atom));
			break;
		case MEDIA_TYPE_AUDIO:
			p = vod_copy(p, smhd_atom, sizeof(smhd_atom));
			break;
		}
		p = vod_copy_atom(p, cur_stream->raw_atoms[RTA_DINF]);

		// moov.trak.minf.stbl
		write_atom_header(p, track_sizes.track_stbl_size, 's', 't', 'b', 'l');
		p = vod_copy_atom(p, cur_stream->raw_atoms[RTA_STSD]);
		p = vod_copy(p, fixed_stbl_atoms, sizeof(fixed_stbl_atoms));
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

uint32_t dash_packager_get_earliest_pres_time(mpeg_stream_metadata_t* stream_metadata)
{
	uint32_t result = stream_metadata->first_frame_time_offset;
	
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
	uint32_t reference_size,
	uint32_t segment_duration)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(sidx_atom_t);

	write_atom_header(p, atom_size, 's', 'i', 'd', 'x');
	write_dword(p, 0);														// version + flags
	write_dword(p, 1);														// reference id
	write_dword(p, stream_metadata->media_info.timescale);					// timescale
	write_dword(p, dash_packager_get_earliest_pres_time(stream_metadata));	// earliest presentation time
	write_dword(p, 0);														// first offset
	write_dword(p, 1);														// reserved + reference count
	write_dword(p, reference_size);											// referenced size
	write_dword(p, stream_metadata->total_frames_duration);					// subsegment duration
	write_dword(p, 0x90000000);												// starts with SAP / SAP type
	return p;
}

static u_char*
dash_packager_write_tfhd_atom(u_char* p, uint32_t track_id)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfhd_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'h', 'd');
	write_dword(p, 0x020000);		// flags
	write_dword(p, track_id);		// track id
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

vod_status_t
dash_packager_build_fragment_header(
	request_context_t* request_context,
	mpeg_stream_metadata_t* stream_metadata,
	uint32_t segment_index,
	uint32_t segment_duration,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size)
{
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
		ATOM_HEADER_SIZE + sizeof(tfhd_atom_t)+
		ATOM_HEADER_SIZE + sizeof(tfdt_atom_t)+
		trun_atom_size;

	moof_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t)+
		traf_atom_size;

	result_size =
		sizeof(styp_atom)+
		ATOM_HEADER_SIZE + sizeof(sidx_atom_t)+
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
			"dash_packager_build_fragment_header: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// styp
	p = vod_copy(result->data, styp_atom, sizeof(styp_atom));

	// sidx
	p = dash_packager_write_sidx_atom(p, stream_metadata, moof_atom_size + mdat_atom_size, segment_duration);

	// moof
	write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

	// moof.mfhd
	p = mp4_builder_write_mfhd_atom(p, segment_index);

	// moof.traf
	write_atom_header(p, traf_atom_size, 't', 'r', 'a', 'f');

	// moof.traf.tfhd
	p = dash_packager_write_tfhd_atom(p, stream_metadata->media_info.track_id);

	// moof.traf.tfdt
	p = dash_packager_write_tfdt_atom(p, dash_packager_get_earliest_pres_time(stream_metadata));

	// moof.traf.trun
	p = mp4_builder_write_trun_atom(
		p, 
		stream_metadata->media_info.media_type, 
		stream_metadata->frames, 
		stream_metadata->frame_count, 
		moof_atom_size);

	// mdat
	write_atom_header(p, mdat_atom_size, 'm', 'd', 'a', 't');

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
