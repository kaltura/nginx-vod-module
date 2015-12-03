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
	"    mediaPresentationDuration=\"PT%uD.%03uDS\"\n"							\
	"    minBufferTime=\"PT%uDS\"\n"											\
	"    profiles=\"%V\">\n"

#define VOD_DASH_MANIFEST_PERIOD_HEADER											\
	"  <Period>\n"

#define VOD_DASH_MANIFEST_PERIOD_DURATION_HEADER								\
	"  <Period id=\"%uD\" duration=\"PT%uD.%03uDS\">\n"

#define VOD_DASH_MANIFEST_VIDEO_HEADER											\
    "    <AdaptationSet\n"														\
    "        id=\"1\"\n"														\
    "        segmentAlignment=\"true\"\n"										\
    "        maxWidth=\"%uD\"\n"												\
    "        maxHeight=\"%uD\"\n"												\
    "        maxFrameRate=\"%uD.%03uD\">\n"

#define VOD_DASH_MANIFEST_VIDEO_PREFIX											\
	"      <Representation\n"													\
    "          id=\"%V\"\n"														\
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
	"          id=\"%V\"\n"														\
    "          mimeType=\"audio/mp4\"\n"										\
    "          codecs=\"%V\"\n"													\
    "          audioSamplingRate=\"%uD\"\n"										\
    "          startWithSAP=\"1\"\n"											\
    "          bandwidth=\"%uD\">\n"

#define VOD_DASH_MANIFEST_AUDIO_SUFFIX											\
	"      </Representation>\n"

#define VOD_DASH_MANIFEST_AUDIO_FOOTER											\
    "    </AdaptationSet>\n"

// SegmentTemplate
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

// SegmentList
#define VOD_DASH_MANIFEST_SEGMENT_LIST_HEADER									\
	"        <SegmentList timescale=\"1000\" duration=\"%ui\">\n"				\
	"          <Initialization sourceURL=\"%V%V-%V.mp4\"/>\n"

#define VOD_DASH_MANIFEST_SEGMENT_URL											\
	"          <SegmentURL media=\"%V%V-%uD-%V.m4s\"/>\n"

#define VOD_DASH_MANIFEST_SEGMENT_LIST_FOOTER									\
	"        </SegmentList>\n"

#define VOD_DASH_MANIFEST_PERIOD_FOOTER											\
	"  </Period>\n"

#define VOD_DASH_MANIFEST_FOOTER												\
    "</MPD>\n"

#define MAX_TRACK_SPEC_LENGTH (sizeof("c-f-v") + 3 * VOD_INT32_LEN)

// init mp4 atoms
typedef struct {
	size_t track_stbl_size;
	size_t track_minf_size;
	size_t track_mdia_size;
	size_t track_trak_size;
} track_sizes_t;

typedef struct {
	track_sizes_t track_sizes;
	size_t moov_atom_size;
	size_t mvex_atom_size;
	size_t stsd_size;
	size_t total_size;
} init_mp4_sizes_t;

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
	uint32_t timescale;
	uint64_t total_frames_duration;
	uint64_t earliest_pres_time;
} sidx_params_t;

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

static bool_t
dash_packager_compare_tracks(uintptr_t bitrate_threshold, const media_info_t* mi1, const media_info_t* mi2)
{
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

static void
dash_packager_get_track_spec(
	vod_str_t* result,
	media_set_t* media_set,
	uint32_t clip_index,
	uint32_t sequence_index,
	uint32_t track_index,
	int media_type_char)
{
	u_char* p = result->data;

	if (media_set->use_discontinuity)
	{
		p = vod_sprintf(p, "c%uD-", clip_index + 1);
	}

	if (media_set->has_multi_sequences && sequence_index != INVALID_SEQUENCE_INDEX)
	{
		p = vod_sprintf(p, "f%uD-", sequence_index + 1);
	}

	p = vod_sprintf(p, "%c%uD", media_type_char, track_index + 1);

	result->len = p - result->data;
}

static uint32_t
dash_packager_get_cur_clip_segment_count(
	segment_durations_t* segment_durations,
	segment_duration_item_t** cur_item_ptr)
{
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	uint32_t result = 0;
	bool_t first_time = TRUE;

	for (cur_item = *cur_item_ptr; cur_item < last_item; cur_item++)
	{
		// stop on discontinuity, will get called again for the next period
		if (cur_item->discontinuity && !first_time)
		{
			break;
		}

		first_time = FALSE;

		result += cur_item->repeat_count;
	}

	*cur_item_ptr = cur_item;

	return result;
}

static u_char* 
dash_packager_write_segment_template(
	u_char* p,
	dash_manifest_config_t* conf,
	segmenter_conf_t* segmenter_conf,
	vod_str_t* base_url)
{
	p = vod_sprintf(p,
		VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FIXED,
		base_url,
		&conf->fragment_file_name_prefix,
		base_url,
		&conf->init_file_name_prefix,
		segmenter_conf->segment_duration);

	return p;
}

static u_char* 
dash_packager_write_segment_timeline(
	u_char* p, 
	dash_manifest_config_t* conf,
	segment_durations_t* segment_durations,
	segment_duration_item_t** cur_item_ptr,
	vod_str_t* base_url)
{
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	uint32_t duration;
	bool_t first_time = TRUE;

	p = vod_sprintf(p,
		VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_HEADER,
		base_url,
		&conf->fragment_file_name_prefix,
		base_url,
		&conf->init_file_name_prefix);

	for (cur_item = *cur_item_ptr; cur_item < last_item; cur_item++)
	{
		// stop on discontinuity, will get called again for the next period
		if (cur_item->discontinuity && !first_time)
		{
			break;
		}

		first_time = FALSE;

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

	*cur_item_ptr = cur_item;

	p = vod_copy(p, VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER, sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER) - 1);

	return p;
}

static u_char*
dash_packager_write_segment_list(
	u_char* p,
	dash_manifest_config_t* conf,
	media_set_t* media_set,
	segmenter_conf_t* segmenter_conf,
	vod_str_t* base_url,
	u_char* base_url_temp_buffer,
	uint32_t clip_index,
	media_sequence_t* cur_sequence,
	media_track_t* cur_track,
	uint32_t segment_count)
{
	int media_type_char = cur_track->media_info.media_type == MEDIA_TYPE_VIDEO ? 'v' : 'a';
	vod_str_t track_spec;
	vod_str_t cur_base_url;
	u_char track_spec_buffer[MAX_TRACK_SPEC_LENGTH];
	uint32_t sequence_index = cur_sequence->index;
	uint32_t i;

	track_spec.data = track_spec_buffer;

	// build the base url
	if (base_url->len != 0)
	{
		cur_base_url.data = base_url_temp_buffer;
		base_url_temp_buffer = vod_copy(base_url_temp_buffer, base_url->data, base_url->len);
		if (cur_track->file_info.uri.len != 0)
		{
			base_url_temp_buffer = vod_copy(base_url_temp_buffer, cur_track->file_info.uri.data, cur_track->file_info.uri.len);
			sequence_index = INVALID_SEQUENCE_INDEX;		// no need to pass the sequence index since we have a direct uri
		}
		else
		{
			base_url_temp_buffer = vod_copy(base_url_temp_buffer, media_set->uri.data, media_set->uri.len);
		}
		*base_url_temp_buffer++ = '/';
		cur_base_url.len = base_url_temp_buffer - cur_base_url.data;
	}
	else
	{
		cur_base_url.data = NULL;
		cur_base_url.len = 0;
	}

	// get the track specification
	dash_packager_get_track_spec(
		&track_spec,
		media_set,
		clip_index,
		sequence_index,
		cur_track->index,
		media_type_char);

	// write the header
	p = vod_sprintf(p,
		VOD_DASH_MANIFEST_SEGMENT_LIST_HEADER,
		segmenter_conf->segment_duration,
		&cur_base_url,
		&conf->init_file_name_prefix,
		&track_spec);

	// write the urls
	for (i = 0; i < segment_count; i++)
	{
		p = vod_sprintf(p,
			VOD_DASH_MANIFEST_SEGMENT_URL,
			&cur_base_url,
			&conf->fragment_file_name_prefix,
			i + 1,
			&track_spec);
	}

	p = vod_copy(p, VOD_DASH_MANIFEST_SEGMENT_LIST_FOOTER, sizeof(VOD_DASH_MANIFEST_SEGMENT_LIST_FOOTER) - 1);

	return p;
}

static u_char* 
dash_packager_write_mpd_period(
	u_char* p,
	u_char* base_url_temp_buffer,
	segment_durations_t* segment_durations,
	segment_duration_item_t** cur_duration_items,
	uint32_t clip_index,
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	segmenter_conf_t* segmenter_conf,
	media_set_t* media_set,
	write_tags_callback_t write_representation_tags,
	void* representation_tags_writer_context)
{
	media_clip_filtered_t* cur_clip;
	media_sequence_t* cur_sequence;
	media_track_t* last_track;
	media_track_t* cur_track;
	vod_str_t representation_id;
	u_char representation_id_buffer[MAX_TRACK_SPEC_LENGTH];
	uint32_t filtered_clip_index;
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	uint32_t max_framerate_duration = 0;
	uint32_t max_framerate_timescale = 0;
	uint32_t segment_count = 0;

	representation_id.data = representation_id_buffer;

	if (media_set->use_discontinuity)
	{
		p = vod_sprintf(p,
			VOD_DASH_MANIFEST_PERIOD_DURATION_HEADER,
			clip_index,
			media_set->durations[clip_index] / 1000,
			media_set->durations[clip_index] % 1000);
	}
	else
	{
		p = vod_copy(p, VOD_DASH_MANIFEST_PERIOD_HEADER, sizeof(VOD_DASH_MANIFEST_PERIOD_HEADER) - 1);
	}

	// Note: clip_index can be greater than clip count when consistentSequenceMediaInfo is true
	filtered_clip_index = clip_index < media_set->clip_count ? clip_index : 0;

	// video adaptation set
	if (media_set->track_count[MEDIA_TYPE_VIDEO] != 0)
	{
		// get the max width, height and frame rate
		cur_track = media_set->filtered_tracks + filtered_clip_index * media_set->total_track_count;
		last_track = cur_track + media_set->total_track_count;
		for (; cur_track < last_track; cur_track++)
		{
			if (cur_track->media_info.media_type != MEDIA_TYPE_VIDEO)
			{
				continue;
			}

			if (cur_track->media_info.u.video.width > max_width)
			{
				max_width = cur_track->media_info.u.video.width;
			}

			if (cur_track->media_info.u.video.height > max_height)
			{
				max_height = cur_track->media_info.u.video.height;
			}

			if (max_framerate_duration == 0 ||
				cur_track->media_info.timescale * max_framerate_duration >
				max_framerate_timescale * cur_track->media_info.min_frame_duration)
			{
				max_framerate_duration = cur_track->media_info.min_frame_duration;
				max_framerate_timescale = cur_track->media_info.timescale;
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
		switch (conf->manifest_format)
		{
		case FORMAT_SEGMENT_TEMPLATE:
			p = dash_packager_write_segment_template(
				p,
				conf,
				segmenter_conf,
				base_url);
			break;

		case FORMAT_SEGMENT_TIMELINE:
			p = dash_packager_write_segment_timeline(
				p,
				conf,
				&segment_durations[MEDIA_TYPE_VIDEO],
				&cur_duration_items[MEDIA_TYPE_VIDEO],
				base_url);
			break;

		case FORMAT_SEGMENT_LIST:
			if (media_set->use_discontinuity)
			{
				segment_count = dash_packager_get_cur_clip_segment_count(
					&segment_durations[MEDIA_TYPE_VIDEO],
					&cur_duration_items[MEDIA_TYPE_VIDEO]);
			}
			else
			{
				segment_count = segment_durations[MEDIA_TYPE_VIDEO].segment_count;
			}
			break;
		}

		// print the representations
		for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
		{
			cur_clip = &cur_sequence->filtered_clips[filtered_clip_index];
			last_track = cur_clip->last_track;
			for (cur_track = cur_clip->first_track; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type != MEDIA_TYPE_VIDEO)
				{
					continue;
				}

				dash_packager_get_track_spec(
					&representation_id, media_set, clip_index, cur_sequence->index, cur_track->index, 'v');

				p = vod_sprintf(p,
					VOD_DASH_MANIFEST_VIDEO_PREFIX,
					&representation_id,
					&cur_track->media_info.codec_name,
					(uint32_t)cur_track->media_info.u.video.width,
					(uint32_t)cur_track->media_info.u.video.height,
					(uint32_t)(cur_track->media_info.timescale / cur_track->media_info.min_frame_duration),
					(uint32_t)(((uint64_t)cur_track->media_info.timescale * 1000) / cur_track->media_info.min_frame_duration % 1000),
					cur_track->media_info.bitrate
					);

				if (conf->manifest_format == FORMAT_SEGMENT_LIST)
				{
					p = dash_packager_write_segment_list(
						p,
						conf,
						media_set,
						segmenter_conf,
						base_url,
						base_url_temp_buffer,
						clip_index,
						cur_sequence,
						cur_track,
						segment_count);
				}

				// write any additional tags
				if (write_representation_tags != NULL)
				{
					p = write_representation_tags(representation_tags_writer_context, p, cur_track);
				}

				p = vod_copy(p, VOD_DASH_MANIFEST_VIDEO_SUFFIX, sizeof(VOD_DASH_MANIFEST_VIDEO_SUFFIX) - 1);
			}
		}

		// print the footer
		p = vod_copy(p, VOD_DASH_MANIFEST_VIDEO_FOOTER, sizeof(VOD_DASH_MANIFEST_VIDEO_FOOTER) - 1);
	}

	// audio adaptation set
	if (media_set->track_count[MEDIA_TYPE_AUDIO] != 0)
	{
		// print the header
		p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_HEADER, sizeof(VOD_DASH_MANIFEST_AUDIO_HEADER) - 1);

		// print the segment template
		switch (conf->manifest_format)
		{
		case FORMAT_SEGMENT_TEMPLATE:
			p = dash_packager_write_segment_template(
				p,
				conf,
				segmenter_conf,
				base_url);
			break;

		case FORMAT_SEGMENT_TIMELINE:
			p = dash_packager_write_segment_timeline(
				p,
				conf,
				&segment_durations[MEDIA_TYPE_AUDIO],
				&cur_duration_items[MEDIA_TYPE_AUDIO],
				base_url);
			break;

		case FORMAT_SEGMENT_LIST:
			if (media_set->use_discontinuity)
			{
				segment_count = dash_packager_get_cur_clip_segment_count(
					&segment_durations[MEDIA_TYPE_AUDIO],
					&cur_duration_items[MEDIA_TYPE_AUDIO]);
			}
			else
			{
				segment_count = segment_durations[MEDIA_TYPE_AUDIO].segment_count;
			}
			break;
		}
		// print the representations
		for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
		{
			cur_clip = &cur_sequence->filtered_clips[filtered_clip_index];
			last_track = cur_clip->last_track;
			for (cur_track = cur_clip->first_track; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type != MEDIA_TYPE_AUDIO)
				{
					continue;
				}

				dash_packager_get_track_spec(
					&representation_id, media_set, clip_index, cur_sequence->index, cur_track->index, 'a');

				p = vod_sprintf(p,
					VOD_DASH_MANIFEST_AUDIO_PREFIX,
					&representation_id,
					&cur_track->media_info.codec_name,
					cur_track->media_info.u.audio.sample_rate,
					cur_track->media_info.bitrate);

				if (conf->manifest_format == FORMAT_SEGMENT_LIST)
				{
					p = dash_packager_write_segment_list(
						p,
						conf,
						media_set,
						segmenter_conf,
						base_url,
						base_url_temp_buffer,
						clip_index,
						cur_sequence,
						cur_track,
						segment_count);
				}

				// write any additional tags
				if (write_representation_tags != NULL)
				{
					p = write_representation_tags(representation_tags_writer_context, p, cur_track);
				}

				p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_SUFFIX, sizeof(VOD_DASH_MANIFEST_AUDIO_SUFFIX) - 1);
			}
		}

		// print the footer
		p = vod_copy(p, VOD_DASH_MANIFEST_AUDIO_FOOTER, sizeof(VOD_DASH_MANIFEST_AUDIO_FOOTER) - 1);
	}

	p = vod_copy(p, VOD_DASH_MANIFEST_PERIOD_FOOTER, sizeof(VOD_DASH_MANIFEST_PERIOD_FOOTER) - 1);

	return p;
}

static size_t
dash_packager_get_segment_list_total_size(
	dash_manifest_config_t* conf,
	media_set_t* media_set, 
	segment_durations_t* segment_durations, 
	vod_str_t* base_url, 
	size_t* base_url_temp_buffer_size)
{
	segment_duration_item_t* cur_duration_item;
	media_track_t* last_track;
	media_track_t* cur_track;
	uint32_t filtered_clip_index;
	uint32_t period_count = media_set->use_discontinuity ? media_set->total_clip_count : 1;
	uint32_t segment_count;
	uint32_t clip_index;
	uint32_t media_type;
	size_t base_url_len;
	size_t result = 0;

	for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
	{
		if (media_set->track_count[media_type] == 0)
		{
			continue;
		}

		cur_duration_item = segment_durations[media_type].items;

		for (clip_index = 0; clip_index < period_count; clip_index++)
		{
			filtered_clip_index = clip_index < media_set->clip_count ? clip_index : 0;

			if (media_set->use_discontinuity)
			{
				segment_count = dash_packager_get_cur_clip_segment_count(
					&segment_durations[media_type],
					&cur_duration_item);
			}
			else
			{
				segment_count = segment_durations[media_type].segment_count;
			}

			cur_track = media_set->filtered_tracks + filtered_clip_index * media_set->total_track_count;
			last_track = cur_track + media_set->total_track_count;
			for (; cur_track < last_track; cur_track++)
			{
				if (cur_track->media_info.media_type != media_type)
				{
					continue;
				}

				if (base_url->len != 0)
				{
					base_url_len = base_url->len + 1;
					if (cur_track->file_info.uri.len != 0)
					{
						base_url_len += cur_track->file_info.uri.len;
					}
					else
					{
						base_url_len += media_set->uri.len;
					}

					if (base_url_len > *base_url_temp_buffer_size)
					{
						*base_url_temp_buffer_size = base_url_len;
					}
				}
				else
				{
					base_url_len = 0;
				}

				result += 
					sizeof(VOD_DASH_MANIFEST_SEGMENT_LIST_HEADER) - 1 + VOD_INT64_LEN + base_url_len + conf->init_file_name_prefix.len + MAX_TRACK_SPEC_LENGTH + 
					(sizeof(VOD_DASH_MANIFEST_SEGMENT_URL) - 1 + base_url_len + conf->fragment_file_name_prefix.len + VOD_INT32_LEN + MAX_TRACK_SPEC_LENGTH) * segment_count + 
					sizeof(VOD_DASH_MANIFEST_SEGMENT_LIST_FOOTER) - 1;
			}
		}
	}

	return result;
}

static void 
dash_packager_remove_redundant_tracks(
	vod_uint_t duplicate_bitrate_threshold,
	media_set_t* media_set)
{
	media_track_t* track1;
	media_track_t* track2;
	media_track_t* last_track;
	uint32_t clip_index;

	last_track = media_set->filtered_tracks + media_set->total_track_count;
	for (track1 = media_set->filtered_tracks + 1; track1 < last_track; track1++)
	{
		for (track2 = media_set->filtered_tracks; track2 < track1; track2++)
		{
			if (track1->media_info.media_type != track2->media_info.media_type)
			{
				continue;
			}

			if (!dash_packager_compare_tracks(
				duplicate_bitrate_threshold,
				&track1->media_info,
				&track2->media_info))
			{
				continue;
			}

			// remove the track from all clips
			media_set->track_count[track1->media_info.media_type]--;
		
			for (clip_index = 0; clip_index < media_set->clip_count; clip_index++)
			{
				track1[clip_index * media_set->total_track_count].media_info.media_type = MEDIA_TYPE_NONE;
			}
			break;
		}
	}
}

vod_status_t 
dash_packager_build_mpd(
	request_context_t* request_context, 
	dash_manifest_config_t* conf,
	vod_str_t* base_url,
	segmenter_conf_t* segmenter_conf,
	media_set_t* media_set,
	size_t representation_tags_size,
	write_tags_callback_t write_representation_tags,
	void* representation_tags_writer_context,
	vod_str_t* result)
{
	segment_durations_t segment_durations[MEDIA_TYPE_COUNT];
	segment_duration_item_t* cur_duration_items[MEDIA_TYPE_COUNT];
	size_t base_url_temp_buffer_size = 0;
	size_t base_period_size;
	size_t result_size;
	size_t urls_length;
	uint32_t period_count = media_set->use_discontinuity ? media_set->total_clip_count : 1;
	uint32_t clip_index;
	uint32_t media_type;
	vod_status_t rc;
	u_char* base_url_temp_buffer = NULL;
	u_char* p;

	// get segment durations and count for each media type
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
	}

	// remove redundant tracks
	dash_packager_remove_redundant_tracks(
		conf->duplicate_bitrate_threshold,
		media_set);

	// calculate the total size
	urls_length = 2 * base_url->len + conf->init_file_name_prefix.len + conf->fragment_file_name_prefix.len;

	base_period_size =
		sizeof(VOD_DASH_MANIFEST_PERIOD_DURATION_HEADER) - 1 + 3 * VOD_INT32_LEN +
			sizeof(VOD_DASH_MANIFEST_VIDEO_HEADER) - 1 + 4 * VOD_INT32_LEN + 
				media_set->track_count[MEDIA_TYPE_VIDEO] * (
				sizeof(VOD_DASH_MANIFEST_VIDEO_PREFIX) - 1 + MAX_TRACK_SPEC_LENGTH + MAX_CODEC_NAME_SIZE + 5 * VOD_INT32_LEN +
				sizeof(VOD_DASH_MANIFEST_VIDEO_SUFFIX) - 1) +
			sizeof(VOD_DASH_MANIFEST_VIDEO_FOOTER) - 1 +
			sizeof(VOD_DASH_MANIFEST_AUDIO_HEADER) - 1 + 
				media_set->track_count[MEDIA_TYPE_AUDIO] * (
				sizeof(VOD_DASH_MANIFEST_AUDIO_PREFIX) - 1 + MAX_TRACK_SPEC_LENGTH + MAX_CODEC_NAME_SIZE + 2 * VOD_INT32_LEN +
				sizeof(VOD_DASH_MANIFEST_AUDIO_SUFFIX) - 1) +
			sizeof(VOD_DASH_MANIFEST_AUDIO_FOOTER) - 1 +
		sizeof(VOD_DASH_MANIFEST_PERIOD_FOOTER) - 1 +
		representation_tags_size;

	result_size =
		sizeof(VOD_DASH_MANIFEST_HEADER) - 1 + 3 * VOD_INT32_LEN + conf->profiles.len +
			base_period_size * period_count +
		sizeof(VOD_DASH_MANIFEST_FOOTER);

	switch (conf->manifest_format)
	{
	case FORMAT_SEGMENT_TEMPLATE:
		result_size += 
			(sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FIXED) - 1 + urls_length + VOD_INT64_LEN) * MEDIA_TYPE_COUNT * period_count;
		break;

	case FORMAT_SEGMENT_TIMELINE:
		for (media_type = 0; media_type < MEDIA_TYPE_COUNT; media_type++)
		{
			if (media_set->track_count[media_type] == 0)
			{
				continue;
			}

			result_size += 
				(sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_HEADER) - 1 + urls_length +
				sizeof(VOD_DASH_MANIFEST_SEGMENT_TEMPLATE_FOOTER) - 1) * period_count +
				(sizeof(VOD_DASH_MANIFEST_SEGMENT_REPEAT) - 1 + 2 * VOD_INT32_LEN) * segment_durations[media_type].item_count;
		}
		break;

	case FORMAT_SEGMENT_LIST:
		result_size += dash_packager_get_segment_list_total_size(
			conf,
			media_set,
			segment_durations,
			base_url, 
			&base_url_temp_buffer_size);
		break;
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dash_packager_build_mpd: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	if (base_url_temp_buffer_size != 0)
	{
		base_url_temp_buffer = vod_alloc(request_context->pool, base_url_temp_buffer_size);
		if (base_url_temp_buffer == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"dash_packager_build_mpd: vod_alloc failed (2)");
			return VOD_ALLOC_FAILED;
		}
	}
	
	// print the manifest header
	p = vod_sprintf(result->data, 
		VOD_DASH_MANIFEST_HEADER,
		(uint32_t)(media_set->total_duration / 1000),
		(uint32_t)(media_set->total_duration % 1000),
		(uint32_t)(segmenter_conf->max_segment_duration / 1000), 
		&conf->profiles);

	cur_duration_items[MEDIA_TYPE_VIDEO] = segment_durations[MEDIA_TYPE_VIDEO].items;
	cur_duration_items[MEDIA_TYPE_AUDIO] = segment_durations[MEDIA_TYPE_AUDIO].items;

	for (clip_index = 0; clip_index < period_count; clip_index++)
	{
		p = dash_packager_write_mpd_period(
			p,
			base_url_temp_buffer,
			segment_durations,
			cur_duration_items,
			clip_index,
			conf,
			base_url,
			segmenter_conf,
			media_set,
			write_representation_tags,
			representation_tags_writer_context);
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
dash_packager_get_track_sizes(media_track_t* cur_track, size_t stsd_size, track_sizes_t* result)
{
	result->track_stbl_size = ATOM_HEADER_SIZE + stsd_size + sizeof(fixed_stbl_atoms);
	result->track_minf_size = ATOM_HEADER_SIZE + cur_track->raw_atoms[RTA_DINF].size + result->track_stbl_size;
	switch (cur_track->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		result->track_minf_size += sizeof(vmhd_atom);
		break;
	case MEDIA_TYPE_AUDIO:
		result->track_minf_size += sizeof(smhd_atom);
		break;
	}
	result->track_mdia_size = ATOM_HEADER_SIZE + cur_track->raw_atoms[RTA_MDHD].size + cur_track->raw_atoms[RTA_HDLR].size + result->track_minf_size;
	result->track_trak_size = ATOM_HEADER_SIZE + cur_track->raw_atoms[RTA_TKHD].size + result->track_mdia_size;
}

static u_char*
dash_packager_write_trex_atom(u_char* p, uint32_t track_id)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(trex_atom_t);

	write_atom_header(p, atom_size, 't', 'r', 'e', 'x');
	write_be32(p, 0);			// version + flags
	write_be32(p, track_id);	// track id
	write_be32(p, 1);			// default sample description index
	write_be32(p, 0);			// default sample duration
	write_be32(p, 0);			// default sample size
	write_be32(p, 0);			// default sample size
	return p;
}

static u_char* 
dash_packager_write_matrix(u_char* p, int16_t a, int16_t b, int16_t c,
	int16_t d, int16_t tx, int16_t ty)
{
	write_be32(p, a << 16);  // 16.16 format
	write_be32(p, b << 16);  // 16.16 format
	write_be32(p, 0);        // u in 2.30 format
	write_be32(p, c << 16);  // 16.16 format
	write_be32(p, d << 16);  // 16.16 format
	write_be32(p, 0);        // v in 2.30 format
	write_be32(p, tx << 16); // 16.16 format
	write_be32(p, ty << 16); // 16.16 format
	write_be32(p, 1 << 30);  // w in 2.30 format
	return p;
}

static u_char*
dash_packager_write_mvhd_constants(u_char* p)
{
	write_be32(p, 0x00010000);	// preferred rate, 1.0
	write_be16(p, 0x0100);		// volume, full
	write_be16(p, 0);			// reserved
	write_be32(p, 0);			// reserved
	write_be32(p, 0);			// reserved
	p = dash_packager_write_matrix(p, 1, 0, 0, 1, 0, 0);	// matrix
	write_be32(p, 0);			// reserved (preview time)
	write_be32(p, 0);			// reserved (preview duration)
	write_be32(p, 0);			// reserved (poster time)
	write_be32(p, 0);			// reserved (selection time)
	write_be32(p, 0);			// reserved (selection duration)
	write_be32(p, 0);			// reserved (current time)
	return p;
}

static u_char*
dash_packager_write_mvhd_atom(u_char* p, uint32_t timescale, uint32_t duration)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(mvhd_atom_t);

	write_atom_header(p, atom_size, 'm', 'v', 'h', 'd');
	write_be32(p, 0);			// version + flags
	write_be32(p, 0);			// creation time
	write_be32(p, 0);			// modification time
	write_be32(p, timescale);	// timescale
	write_be32(p, duration);	// duration
	p = dash_packager_write_mvhd_constants(p);
	write_be32(p, 0xffffffff); // next track id
	return p;
}

static u_char*
dash_packager_write_mvhd64_atom(u_char* p, uint32_t timescale, uint64_t duration)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(mvhd64_atom_t);

	write_atom_header(p, atom_size, 'm', 'v', 'h', 'd');
	write_be32(p, 0x01000000);	// version + flags
	write_be64(p, 0LL);			// creation time
	write_be64(p, 0LL);			// modification time
	write_be32(p, timescale);	// timescale
	write_be64(p, duration);	// duration
	p = dash_packager_write_mvhd_constants(p);
	write_be32(p, 0xffffffff);	// next track id
	return p;
}

static void
dash_packager_init_mp4_calc_size(
	media_set_t* media_set,
	atom_writer_t* extra_moov_atoms_writer,
	atom_writer_t* stsd_atom_writer, 
	init_mp4_sizes_t* result)
{
	media_track_t* first_track = media_set->sequences[0].filtered_clips[0].first_track;
	
	if (stsd_atom_writer != NULL)
	{
		result->stsd_size = stsd_atom_writer->atom_size;
	}
	else
	{
		result->stsd_size = first_track->raw_atoms[RTA_STSD].size;
	}

	dash_packager_get_track_sizes(first_track, result->stsd_size, &result->track_sizes);

	result->mvex_atom_size = ATOM_HEADER_SIZE + ATOM_HEADER_SIZE + sizeof(trex_atom_t);

	result->moov_atom_size = ATOM_HEADER_SIZE +
		result->track_sizes.track_trak_size +
		result->mvex_atom_size;

	if (media_set->sequences[0].filtered_clips[0].mvhd_atom.ptr != NULL)
	{
		result->moov_atom_size += media_set->sequences[0].filtered_clips[0].mvhd_atom.size;
	}
	else if (media_set->total_duration > UINT_MAX)
	{
		result->moov_atom_size += ATOM_HEADER_SIZE + sizeof(mvhd64_atom_t);
	}
	else
	{
		result->moov_atom_size += ATOM_HEADER_SIZE + sizeof(mvhd_atom_t);
	}

	if (extra_moov_atoms_writer != NULL)
	{
		result->moov_atom_size += extra_moov_atoms_writer->atom_size;
	}

	result->total_size = sizeof(ftyp_atom) + result->moov_atom_size;
}

static u_char*
dash_packager_init_mp4_write(
	u_char* p,
	request_context_t* request_context, 
	media_set_t* media_set, 
	init_mp4_sizes_t* sizes,
	atom_writer_t* extra_moov_atoms_writer,
	atom_writer_t* stsd_atom_writer)
{
	media_track_t* first_track = media_set->sequences[0].filtered_clips[0].first_track;

	// ftyp
	p = vod_copy(p, ftyp_atom, sizeof(ftyp_atom));

	// moov
	write_atom_header(p, sizes->moov_atom_size, 'm', 'o', 'o', 'v');

	// moov.mvhd
	if (media_set->sequences[0].filtered_clips[0].mvhd_atom.ptr != NULL)
	{
		// TODO: adjust the mvhd duration (can change in case of clipping or sequencing)
		p = vod_copy_atom(p, media_set->sequences[0].filtered_clips[0].mvhd_atom);
	}
	else if (media_set->total_duration > UINT_MAX)
	{
		p = dash_packager_write_mvhd64_atom(p, 1000, media_set->total_duration);
	}
	else
	{
		p = dash_packager_write_mvhd_atom(p, 1000, media_set->total_duration);
	}

	// moov.mvex
	write_atom_header(p, sizes->mvex_atom_size, 'm', 'v', 'e', 'x');

	// moov.mvex.trex
	p = dash_packager_write_trex_atom(p, first_track->media_info.track_id);

	// moov.trak
	write_atom_header(p, sizes->track_sizes.track_trak_size, 't', 'r', 'a', 'k');
	p = vod_copy_atom(p, first_track->raw_atoms[RTA_TKHD]);

	// moov.trak.mdia
	write_atom_header(p, sizes->track_sizes.track_mdia_size, 'm', 'd', 'i', 'a');
	p = vod_copy_atom(p, first_track->raw_atoms[RTA_MDHD]);
	p = vod_copy_atom(p, first_track->raw_atoms[RTA_HDLR]);

	// moov.trak.minf
	write_atom_header(p, sizes->track_sizes.track_minf_size, 'm', 'i', 'n', 'f');
	switch (first_track->media_info.media_type)
	{
	case MEDIA_TYPE_VIDEO:
		p = vod_copy(p, vmhd_atom, sizeof(vmhd_atom));
		break;
	case MEDIA_TYPE_AUDIO:
		p = vod_copy(p, smhd_atom, sizeof(smhd_atom));
		break;
	}
	p = vod_copy_atom(p, first_track->raw_atoms[RTA_DINF]);

	// moov.trak.minf.stbl
	write_atom_header(p, sizes->track_sizes.track_stbl_size, 's', 't', 'b', 'l');
	if (stsd_atom_writer != NULL)
	{
		p = stsd_atom_writer->write(stsd_atom_writer->context, p);
	}
	else
	{
		p = vod_copy_atom(p, first_track->raw_atoms[RTA_STSD]);
	}
	p = vod_copy(p, fixed_stbl_atoms, sizeof(fixed_stbl_atoms));

	// moov.xxx
	if (extra_moov_atoms_writer != NULL)
	{
		p = extra_moov_atoms_writer->write(extra_moov_atoms_writer->context, p);
	}

	return p;
}

vod_status_t 
dash_packager_build_init_mp4(
	request_context_t* request_context,
	media_set_t* media_set,
	bool_t size_only,
	atom_writer_t* extra_moov_atoms_writer,
	atom_writer_t* stsd_atom_writer,
	vod_str_t* result)
{
	init_mp4_sizes_t sizes;
	u_char* p;

	// get the result size
	dash_packager_init_mp4_calc_size(
		media_set,
		extra_moov_atoms_writer,
		stsd_atom_writer,
		&sizes);

	// head request optimization
	if (size_only)
	{
		result->len = sizes.total_size;
		return VOD_OK;
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, sizes.total_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dash_packager_build_init_mp4: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// write the init mp4
	p = dash_packager_init_mp4_write(
		result->data,
		request_context,
		media_set,
		&sizes,
		extra_moov_atoms_writer,
		stsd_atom_writer);

	result->len = p - result->data;

	if (result->len != sizes.total_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dash_packager_build_init_mp4: result length %uz different than allocated length %uz",
			result->len, sizes.total_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

// fragment writing code

static uint64_t 
dash_packager_get_earliest_pres_time(media_set_t* media_set, media_track_t* track)
{
	uint64_t result = track->first_frame_time_offset;
	
	if (!media_set->use_discontinuity)
	{
		result += track->clip_sequence_offset;
	}

	if (track->frame_count > 0)
	{
		result += track->first_frame[0].pts_delay;
	}
	return result;
}

static u_char*
dash_packager_write_sidx_atom(
	u_char* p,
	sidx_params_t* sidx_params,
	uint32_t reference_size)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(sidx_atom_t);

	write_atom_header(p, atom_size, 's', 'i', 'd', 'x');
	write_be32(p, 0);					// version + flags
	write_be32(p, 1);					// reference id
	write_be32(p, sidx_params->timescale);			// timescale
	write_be32(p, sidx_params->earliest_pres_time);	// earliest presentation time
	write_be32(p, 0);					// first offset
	write_be32(p, 1);					// reserved + reference count
	write_be32(p, reference_size);		// referenced size
	write_be32(p, sidx_params->total_frames_duration);		// subsegment duration
	write_be32(p, 0x90000000);			// starts with SAP / SAP type
	return p;
}

static u_char*
dash_packager_write_sidx64_atom(
	u_char* p,
	sidx_params_t* sidx_params,
	uint32_t reference_size)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(sidx64_atom_t);

	write_atom_header(p, atom_size, 's', 'i', 'd', 'x');
	write_be32(p, 0x01000000);			// version + flags
	write_be32(p, 1);					// reference id
	write_be32(p, sidx_params->timescale);			// timescale
	write_be64(p, sidx_params->earliest_pres_time);	// earliest presentation time
	write_be64(p, 0LL);					// first offset
	write_be32(p, 1);					// reserved + reference count
	write_be32(p, reference_size);		// referenced size
	write_be32(p, sidx_params->total_frames_duration);		// subsegment duration
	write_be32(p, 0x90000000);			// starts with SAP / SAP type
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
	write_be32(p, flags);			// flags
	write_be32(p, track_id);		// track id
	if (sample_description_index > 0)
	{
		write_be32(p, sample_description_index);
	}
	return p;
}

static u_char*
dash_packager_write_tfdt_atom(u_char* p, uint32_t earliest_pres_time)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfdt_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'd', 't');
	write_be32(p, 0);
	write_be32(p, earliest_pres_time);
	return p;
}

static u_char*
dash_packager_write_tfdt64_atom(u_char* p, uint64_t earliest_pres_time)
{
	size_t atom_size = ATOM_HEADER_SIZE + sizeof(tfdt64_atom_t);

	write_atom_header(p, atom_size, 't', 'f', 'd', 't');
	write_be32(p, 0x01000000);			// version = 1
	write_be64(p, earliest_pres_time);
	return p;
}

static void
dash_packager_init_sidx_params(media_set_t* media_set, media_sequence_t* sequence, sidx_params_t* result)
{
	media_clip_filtered_t* cur_clip;
	media_track_t* track;
	uint64_t earliest_pres_time;
	uint64_t total_frames_duration;
	uint32_t timescale;

	// initialize according to the first clip
	cur_clip = sequence->filtered_clips;
	track = cur_clip->first_track;
	total_frames_duration = track->total_frames_duration;
	earliest_pres_time = dash_packager_get_earliest_pres_time(media_set, track);
	timescale = track->media_info.timescale;
	cur_clip++;

	for (; cur_clip < sequence->filtered_clips_end; cur_clip++)
	{
		track = cur_clip->first_track;

		if (timescale > track->media_info.timescale)
		{
			// scale the track duration to timescale
			total_frames_duration += rescale_time(track->total_frames_duration, track->media_info.timescale, timescale);
			continue;
		}

		if (timescale < track->media_info.timescale)
		{
			// rescale to track->media_info.timescale
			total_frames_duration = rescale_time(total_frames_duration, timescale, track->media_info.timescale);
			earliest_pres_time = rescale_time(earliest_pres_time, timescale, track->media_info.timescale);
			timescale = track->media_info.timescale;
		}

		// same timescale, just add
		total_frames_duration += track->total_frames_duration;
	}

	result->total_frames_duration = total_frames_duration;
	result->earliest_pres_time = earliest_pres_time;
	result->timescale = timescale;
}

vod_status_t
dash_packager_build_fragment_header(
	request_context_t* request_context,
	media_set_t* media_set,
	uint32_t segment_index,
	uint32_t sample_description_index,
	dash_fragment_header_extensions_t* extensions,
	bool_t size_only,
	vod_str_t* result,
	size_t* total_fragment_size)
{
	media_sequence_t* sequence = &media_set->sequences[0];
	media_track_t* first_track = sequence->filtered_clips[0].first_track;
	uint64_t earliest_pres_time = dash_packager_get_earliest_pres_time(media_set, first_track);
	sidx_params_t sidx_params;
	size_t first_frame_offset;
	size_t mdat_atom_size;
	size_t trun_atom_size;
	size_t tfhd_atom_size;
	size_t moof_atom_size;
	size_t traf_atom_size;
	size_t result_size;
	u_char* p;

	// calculate sizes
	dash_packager_init_sidx_params(media_set, sequence, &sidx_params);

	mdat_atom_size = ATOM_HEADER_SIZE + sequence->total_frame_size;
	trun_atom_size = mp4_builder_get_trun_atom_size(first_track->media_info.media_type, sequence->total_frame_count);

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
		extensions->extra_traf_atoms_size;

	moof_atom_size =
		ATOM_HEADER_SIZE +
		ATOM_HEADER_SIZE + sizeof(mfhd_atom_t)+
		traf_atom_size;

	*total_fragment_size = 
		sizeof(styp_atom) +
		ATOM_HEADER_SIZE + (sidx_params.earliest_pres_time > UINT_MAX ? sizeof(sidx64_atom_t) : sizeof(sidx_atom_t)) +
		moof_atom_size +
		mdat_atom_size;

	result_size = *total_fragment_size - sequence->total_frame_size;

	// head request optimization
	if (size_only)
	{
		return VOD_OK;
	}

	// allocate the buffer
	p = vod_alloc(request_context->pool, result_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dash_packager_build_fragment_header: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->data = p;

	// styp
	p = vod_copy(p, styp_atom, sizeof(styp_atom));

	// sidx
	if (sidx_params.earliest_pres_time > UINT_MAX)
	{
		p = dash_packager_write_sidx64_atom(p, &sidx_params, moof_atom_size + mdat_atom_size);
	}
	else
	{
		p = dash_packager_write_sidx_atom(p, &sidx_params, moof_atom_size + mdat_atom_size);
	}

	// moof
	write_atom_header(p, moof_atom_size, 'm', 'o', 'o', 'f');

	// moof.mfhd
	p = mp4_builder_write_mfhd_atom(p, segment_index);

	// moof.traf
	write_atom_header(p, traf_atom_size, 't', 'r', 'a', 'f');

	// moof.traf.tfhd
	p = dash_packager_write_tfhd_atom(p, first_track->media_info.track_id, sample_description_index);

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

	p = mp4_builder_write_trun_atom(
		p, 
		sequence, 
		first_frame_offset);

	// moof.traf.xxx
	if (extensions->write_extra_traf_atoms_callback != NULL)
	{
		p = extensions->write_extra_traf_atoms_callback(extensions->write_extra_traf_atoms_context, p, moof_atom_size);
	}

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
