#include "hds_manifest.h"
#include "hds_fragment.h"
#include "hds_amf0_encoder.h"
#include "../manifest_utils.h"
#include "../mp4/mp4_defs.h"
#include "../mp4/mp4_write_stream.h"
#include "../udrm.h"

// manifest constants
#define HDS_MANIFEST_HEADER								\
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"		\
	"<manifest\n"										\
	"  xmlns=\"http://ns.adobe.com/f4m/1.0\">\n"		\
	"  <id>%V</id>\n"

#define HDS_MANIFEST_HEADER_BASE_URL					\
	"  <baseURL>%V</baseURL>\n"

#define HDS_MANIFEST_HEADER_VOD							\
	"  <duration>%uD.%03uD</duration>\n"				\
	"  <streamType>recorded</streamType>\n"

#define HDS_MANIFEST_HEADER_LIVE						\
	"  <streamType>live</streamType>\n"

#define HDS_MANIFEST_HEADER_LANG						\
	"  <label>%V</label>\n"								\
	"  <lang>%V</lang>\n"

#define HDS_BOOTSTRAP_LIVE_PREFIX						\
	"  <bootstrapInfo\n"								\
	"    profile=\"named\"\n"							\
	"    id=\"bootstrap%uD\"\n"							\
	"    url=\""

#define HDS_BOOTSTRAP_LIVE_SUFFIX						\
	".abst\"/>\n"

#define HDS_BOOTSTRAP_VOD_HEADER						\
	"  <bootstrapInfo\n"								\
	"    profile=\"named\"\n"							\
	"    id=\"bootstrap%uD\">"

#define HDS_BOOTSTRAP_VOD_FOOTER						\
	"</bootstrapInfo>\n"

#define HDS_DRM_ADDITIONAL_HEADER_PREFIX				\
	"  <drmAdditionalHeader\n"							\
	"    id=\"drmMetadata%uD\">\n      "

#define HDS_DRM_ADDITIONAL_HEADER_SUFFIX				\
	"\n  </drmAdditionalHeader>\n"

#define HDS_MEDIA_HEADER_PREFIX_VIDEO					\
	"  <media\n"										\
	"    bitrate=\"%uD\"\n"								\
	"    width=\"%uD\"\n"								\
	"    height=\"%uD\"\n"								\
	"    url=\""

#define HDS_MEDIA_HEADER_PREFIX_AUDIO					\
	"  <media\n"										\
	"    bitrate=\"%uD\"\n"								\
	"    url=\""

#define HDS_MEDIA_HEADER_PREFIX_AUDIO_LANG				\
	"  <media\n"										\
	"    bitrate=\"%uD\"\n"								\
	"    type=\"audio\"\n"								\
	"    label=\"%V\"\n"								\
	"    lang=\"%V\"\n"									\
	"    alternate=\"true\"\n"							\
	"    url=\""

#define HDS_MEDIA_HEADER_SUFFIX							\
	"\"\n"												\
	"    bootstrapInfoId=\"bootstrap%uD\">\n"			\
	"    <metadata>"

#define HDS_MEDIA_HEADER_SUFFIX_DRM						\
	"\"\n"												\
	"    bootstrapInfoId=\"bootstrap%uD\"\n"			\
	"    drmAdditionalHeaderId=\"drmMetadata%uD\">\n"	\
	"    <metadata>"

#define HDS_MEDIA_FOOTER								\
	"</metadata>\n"										\
	"  </media>\n"

#define HDS_MANIFEST_FOOTER								\
	"</manifest>\n"


#define AFRT_BASE_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(afrt_atom_t))
#define ASRT_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(asrt_atom_t) + sizeof(asrt_entry_t))
#define ABST_BASE_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(abst_atom_t) + ASRT_ATOM_SIZE + sizeof(u_char) + AFRT_BASE_ATOM_SIZE)

// typedefs
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char bootstrap_version[4];
	u_char extra_flags[1];				// 2 bit profile, 1 bit live, 1 bit update
	u_char timescale[4];
	u_char current_media_time[8];
	u_char smpte_offset[8];
	u_char movie_identifier[1];			// this struct assumes the string is empty
	u_char server_entries[1];
	u_char quality_entries[1];
	u_char drm_data[1];					// this struct assumes the string is empty
	u_char metadata[1];					// this struct assumes the string is empty
	u_char segment_run_table_count[1];
} abst_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char quality_entries[1];
	u_char segment_run_entries[4];
} asrt_atom_t;

typedef struct {
	u_char first_segment[4];
	u_char fragments_per_segment[4];
} asrt_entry_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char timescale[4];
	u_char quality_entries[1];
	u_char fragment_run_entries[4];
} afrt_atom_t;

typedef struct {
	u_char first_fragment[4];
	u_char first_fragment_timestamp[8];
	u_char fragment_duration[4];
} afrt_entry_t;

typedef struct {
	segment_durations_t durations;
	uint32_t zero_segments;
} hds_segment_durations_t;

static void
hds_scale_segment_durations(hds_segment_durations_t* segments)
{
	segment_durations_t* segment_durations = &segments->durations;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	segment_duration_item_t* cur_item;

	segments->zero_segments = 0;

	segment_durations->end_time = hds_rescale_millis(segment_durations->end_time);
	for (cur_item = segment_durations->items; cur_item < last_item; cur_item++)
	{
		cur_item->time = segment_durations->timescale == 1000 ? hds_rescale_millis(cur_item->time) :				// special case for 1000 to prevent overflow in time
			rescale_time(cur_item->time, segment_durations->timescale, HDS_TIMESCALE);
		cur_item->duration = rescale_time(cur_item->duration, segment_durations->timescale, HDS_TIMESCALE);

		if (cur_item->duration == 0)
		{
			segments->zero_segments++;
		}
	}
}

static size_t
hds_get_abst_atom_size(media_set_t* media_set, hds_segment_durations_t* segments)
{
	uint32_t fragment_run_entries;
	uint32_t base_size;

	base_size = ABST_BASE_ATOM_SIZE;
	fragment_run_entries = segments->durations.item_count;
	if (media_set->presentation_end)
	{
		fragment_run_entries++;				// zero entry
		base_size += sizeof(u_char);		// discontinuity indicator
	}

	fragment_run_entries += segments->durations.discontinuities;			// zero entry
	base_size += sizeof(u_char) * (segments->durations.discontinuities + segments->zero_segments);	// discontinuity indicator

	return base_size + fragment_run_entries * sizeof(afrt_entry_t);
}

static u_char*
hds_write_abst_atom(
	u_char* p,
	media_set_t* media_set,
	hds_segment_durations_t* segments)
{
	segment_durations_t* segment_durations = &segments->durations;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	uint64_t timestamp;
	uint32_t fragment_run_entries;
	uint32_t segment_index;
	uint32_t duration;
	size_t afrt_atom_size = AFRT_BASE_ATOM_SIZE;
	size_t asrt_atom_size = ASRT_ATOM_SIZE;
	size_t abst_atom_size = ABST_BASE_ATOM_SIZE;
	size_t extra_afrt_size = 0;

	fragment_run_entries = segment_durations->item_count;
	if (media_set->presentation_end)
	{
		fragment_run_entries++;					// zero entry
		extra_afrt_size += sizeof(u_char);		// discontinuity indicator
	}

	fragment_run_entries += segment_durations->discontinuities;					// zero entry
	extra_afrt_size += fragment_run_entries * sizeof(afrt_entry_t) +
		sizeof(u_char) * (segment_durations->discontinuities + segments->zero_segments);	// discontinuity indicator

	afrt_atom_size += extra_afrt_size;
	abst_atom_size += extra_afrt_size;

	// abst
	write_atom_header(p, abst_atom_size, 'a', 'b', 's', 't');
	write_be32(p, 0);					// version + flags
	write_be32(p, 1);					// bootstrap info version
	*p++ = (media_set->type == MEDIA_SET_LIVE ? 0x20 : 0);	// profile, live, update
	write_be32(p, HDS_TIMESCALE);		// timescale
	write_be64(p, segment_durations->end_time);	// current media time
	write_be64(p, 0LL);					// smpte offset
	*p++ = 0;							// movie identifier
	*p++ = 0;							// server entries
	*p++ = 0;							// quality entries
	*p++ = 0;							// drm data
	*p++ = 0;							// metadata
	*p++ = 1;							// segment run table count

	// abst.asrt
	write_atom_header(p, asrt_atom_size, 'a', 's', 'r', 't');
	write_be32(p, 0);					// version + flags
	*p++ = 0;							// quality entries
	write_be32(p, 1);					// segment run entries
	// entry #1
	write_be32(p, 1);					// first segment
	write_be32(p, segment_durations->segment_count);		// fragments per segment

	// abst
	*p++ = 1;							// fragment run table count

	// abst.afrt
	write_atom_header(p, afrt_atom_size, 'a', 'f', 'r', 't');
	write_be32(p, 0);					// version + flags
	write_be32(p, HDS_TIMESCALE);		// timescale
	*p++ = 0;							// quality entries
	write_be32(p, fragment_run_entries);	// fragment run entries

	// write the afrt entries
	for (cur_item = segment_durations->items; cur_item < last_item; cur_item++)
	{
		segment_index = cur_item->segment_index + 1;
		timestamp = cur_item->time;
		duration = cur_item->duration;

		write_be32(p, segment_index);	// first fragment
		write_be64(p, timestamp);		// first fragment timestamp
		write_be32(p, duration);		// fragment duration
		if (duration == 0)
		{
			*p++ = 1;					// discontinuity indicator (1 = discontinuity in fragment numbering)
		}

		if (cur_item + 1 < last_item && cur_item[1].discontinuity)
		{
			segment_index += cur_item->repeat_count;
			timestamp += duration * cur_item->repeat_count;
			write_be32(p, segment_index);		// first fragment
			write_be64(p, timestamp);			// first fragment timestamp
			write_be32(p, 0);					// fragment duration
			*p++ = 3;							// discontinuity indicator (3 = discontinuity in both timestamps and fragment numbering)
		}
	}

	if (media_set->presentation_end)
	{
		// last entry
		write_be32(p, 0);					// first fragment
		write_be64(p, 0LL);					// first fragment timestamp
		write_be32(p, 0);					// fragment duration
		*p++ = 0;							// discontinuity indicator (0 = end of presentation)
	}

	return p;
}

static u_char*
hds_write_base64_abst_atom(u_char* p, u_char* temp_buffer, media_set_t* media_set, hds_segment_durations_t* segment_durations)
{
	vod_str_t binary;
	vod_str_t base64;
	
	binary.data = temp_buffer;
	binary.len = hds_write_abst_atom(binary.data, media_set, segment_durations) - binary.data;

	base64.data = p;

	vod_encode_base64(&base64, &binary);

	return p + base64.len;
}

vod_status_t
hds_packager_build_bootstrap(
	request_context_t* request_context,
	media_set_t* media_set,
	vod_str_t* result)
{
	hds_segment_durations_t segment_durations;
	segmenter_conf_t* segmenter_conf = media_set->segmenter_conf;
	vod_status_t rc;
	size_t result_size;

	rc = segmenter_conf->get_segment_durations(
		request_context,
		segmenter_conf,
		media_set,
		NULL,
		MEDIA_TYPE_NONE,
		&segment_durations.durations);
	if (rc != VOD_OK)
	{
		return rc;
	}

	hds_scale_segment_durations(&segment_durations);

	result_size = hds_get_abst_atom_size(media_set, &segment_durations);

	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_bootstrap: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	result->len = hds_write_abst_atom(result->data, media_set, &segment_durations) - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"hds_packager_build_bootstrap: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
hds_packager_build_manifest(
	request_context_t* request_context,
	hds_manifest_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* manifest_id,
	media_set_t* media_set,
	bool_t drm_enabled,
	vod_str_t* result)
{
	hds_segment_durations_t* segment_durations;
	adaptation_sets_t adaptation_sets;
	adaptation_set_t* adaptation_set;
	segmenter_conf_t* segmenter_conf = media_set->segmenter_conf;
	media_sequence_t* cur_sequence;
	media_track_t** tracks;
	media_track_t** cur_track_ptr;
	media_track_t* tracks_array[MEDIA_TYPE_COUNT];
	media_track_t* track;
	vod_str_t* drm_metadata;
	uint32_t initial_muxed_tracks;
	uint32_t muxed_tracks;
	uint32_t media_count;
	uint32_t bitrate;
	uint32_t index;
	uint32_t abst_atom_size;
	uint32_t max_abst_atom_size = 0;
	size_t result_size;
	vod_status_t rc;
	u_char* temp_buffer;
	u_char* p;

	// get the adaptations sets
	rc = manifest_utils_get_adaptation_sets(
		request_context, 
		media_set, 
		ADAPTATION_SETS_FLAG_MUXED | 
		ADAPTATION_SETS_FLAG_EXCLUDE_MUXED_AUDIO |
		ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK | 
		ADAPTATION_SETS_FLAG_AVOID_AUDIO_ONLY,
		&adaptation_sets);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate the segment durations
	media_count = adaptation_sets.first->count + adaptation_sets.total_count - 1;
	segment_durations = vod_alloc(request_context->pool, sizeof(segment_durations[0]) * media_count);
	if (segment_durations == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	// calculate the result size
	result_size = 
		sizeof(HDS_MANIFEST_HEADER) - 1 + manifest_id->len + 
		sizeof(HDS_MANIFEST_HEADER_BASE_URL) - 1 + base_url->len +
		sizeof(HDS_MANIFEST_HEADER_LANG) - 1 +
		sizeof(HDS_MANIFEST_FOOTER);

	switch (media_set->type)
	{
	case MEDIA_SET_VOD:
		result_size += 
			sizeof(HDS_MANIFEST_HEADER_VOD) - 1 + 2 * VOD_INT32_LEN + 
			(sizeof(HDS_BOOTSTRAP_VOD_HEADER) - 1 + VOD_INT32_LEN +
			sizeof(HDS_BOOTSTRAP_VOD_FOOTER) - 1) * media_count;
		break;

	case MEDIA_SET_LIVE:
		result_size += 
			sizeof(HDS_MANIFEST_HEADER_LIVE) - 1 + 
			(sizeof(HDS_BOOTSTRAP_LIVE_PREFIX) - 1 + VOD_INT32_LEN +
			conf->bootstrap_file_name_prefix.len +
			MANIFEST_UTILS_TRACKS_SPEC_MAX_SIZE +
			sizeof(HDS_BOOTSTRAP_LIVE_SUFFIX) - 1) * media_count;
		break;
	}

	if (drm_enabled)
	{
		result_size += 
			(sizeof(HDS_DRM_ADDITIONAL_HEADER_PREFIX) - 1 + VOD_INT32_LEN +
			sizeof(HDS_DRM_ADDITIONAL_HEADER_SUFFIX) - 1) * media_count;
	}

	result_size +=
		(vod_max(sizeof(HDS_MEDIA_HEADER_PREFIX_VIDEO) - 1 + 3 * VOD_INT32_LEN, 
			sizeof(HDS_MEDIA_HEADER_PREFIX_AUDIO_LANG) - 1 + VOD_INT32_LEN) +
		conf->fragment_file_name_prefix.len +
		MANIFEST_UTILS_TRACKS_SPEC_MAX_SIZE + 1 +		// 1 = -
		sizeof(HDS_MEDIA_HEADER_SUFFIX_DRM) - 1 + 2 * VOD_INT32_LEN +
		vod_base64_encoded_length(amf0_max_total_size) +
		sizeof(HDS_MEDIA_FOOTER) - 1) * media_count;

	initial_muxed_tracks = adaptation_sets.first->type == ADAPTATION_TYPE_MUXED ? MEDIA_TYPE_COUNT : 1;

	muxed_tracks = initial_muxed_tracks;
	index = 0;
	for (adaptation_set = adaptation_sets.first; adaptation_set < adaptation_sets.last; adaptation_set++)
	{
		if (adaptation_set->type == ADAPTATION_TYPE_MUXED)
		{
			if (adaptation_set->first[MEDIA_TYPE_AUDIO] != NULL)
			{
				result_size += adaptation_set->first[MEDIA_TYPE_AUDIO]->media_info.tags.label.len +
					adaptation_set->first[MEDIA_TYPE_AUDIO]->media_info.tags.lang_str.len;
			}
		}
		else
		{
			result_size += adaptation_set->first[0]->media_info.tags.label.len +
				adaptation_set->first[0]->media_info.tags.lang_str.len;
		}

		for (cur_track_ptr = adaptation_set->first; cur_track_ptr < adaptation_set->last; cur_track_ptr += muxed_tracks)
		{
			if (cur_track_ptr[0] != NULL)
			{
				cur_sequence = cur_track_ptr[0]->file_info.source->sequence;
			}
			else
			{
				cur_sequence = cur_track_ptr[1]->file_info.source->sequence;
			}

			switch (media_set->type)
			{
			case MEDIA_SET_VOD:
				rc = segmenter_conf->get_segment_durations(
					request_context,
					segmenter_conf,
					media_set,
					cur_sequence,		// XXXXX change to work with tracks instead of sequence
					MEDIA_TYPE_NONE,
					&segment_durations[index].durations);
				if (rc != VOD_OK)
				{
					return rc;
				}

				hds_scale_segment_durations(&segment_durations[index]);

				abst_atom_size = hds_get_abst_atom_size(media_set, &segment_durations[index]);
				if (abst_atom_size > max_abst_atom_size)
				{
					max_abst_atom_size = abst_atom_size;
				}

				result_size += vod_base64_encoded_length(abst_atom_size);

				index++;
				break;
			}

			if (drm_enabled)
			{
				drm_metadata = &((drm_info_t*)cur_sequence->drm_info)->pssh_array.first->data;

				result_size += drm_metadata->len;
			}
		}

		muxed_tracks = 1;
	}

	// allocate the buffers
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	temp_buffer = vod_alloc(request_context->pool, vod_max(amf0_max_total_size, max_abst_atom_size));
	if (temp_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (3)");
		return VOD_ALLOC_FAILED;
	}

	// print the manifest header
	p = vod_sprintf(result->data, HDS_MANIFEST_HEADER, manifest_id);

	if (base_url->len != 0)
	{
		p = vod_sprintf(p, HDS_MANIFEST_HEADER_BASE_URL, base_url);
	}

	switch (media_set->type)
	{
	case MEDIA_SET_VOD:
		p = vod_sprintf(p, HDS_MANIFEST_HEADER_VOD, 
			(uint32_t)(media_set->timing.total_duration / 1000),
			(uint32_t)(media_set->timing.total_duration % 1000));
		break;

	case MEDIA_SET_LIVE:
		p = vod_copy(p, HDS_MANIFEST_HEADER_LIVE, sizeof(HDS_MANIFEST_HEADER_LIVE) - 1);
		break;
	}

	if (adaptation_sets.total_count > 1)
	{
		if (adaptation_sets.first->type == ADAPTATION_TYPE_MUXED)
		{
			track = adaptation_sets.first->first[MEDIA_TYPE_AUDIO];
		}
		else
		{
			track = adaptation_sets.first->first[0];
		}

		p = vod_sprintf(p, HDS_MANIFEST_HEADER_LANG, 
			&track->media_info.tags.label,
			&track->media_info.tags.lang_str);
	}

	// bootstrap tags
	muxed_tracks = initial_muxed_tracks;
	tracks_array[MEDIA_TYPE_SUBTITLE] = NULL;
	index = 0;
	for (adaptation_set = adaptation_sets.first; adaptation_set < adaptation_sets.last; adaptation_set++)
	{
		for (cur_track_ptr = adaptation_set->first; cur_track_ptr < adaptation_set->last; cur_track_ptr += muxed_tracks)
		{
			switch (media_set->type)
			{
			case MEDIA_SET_VOD:
				p = vod_sprintf(p, HDS_BOOTSTRAP_VOD_HEADER, index);
				p = hds_write_base64_abst_atom(p, temp_buffer, media_set, &segment_durations[index]);
				p = vod_copy(p, HDS_BOOTSTRAP_VOD_FOOTER, sizeof(HDS_BOOTSTRAP_VOD_FOOTER) - 1);
				break;

			case MEDIA_SET_LIVE:
				// get the tracks
				if (muxed_tracks == 1)
				{
					if (cur_track_ptr[0]->media_info.media_type == MEDIA_TYPE_VIDEO)
					{
						tracks_array[MEDIA_TYPE_VIDEO] = cur_track_ptr[0];
						tracks_array[MEDIA_TYPE_AUDIO] = NULL;
					}
					else
					{
						tracks_array[MEDIA_TYPE_VIDEO] = NULL;
						tracks_array[MEDIA_TYPE_AUDIO] = cur_track_ptr[0];
					}
					tracks = tracks_array;
				}
				else
				{
					tracks = cur_track_ptr;
				}

				p = vod_sprintf(p, HDS_BOOTSTRAP_LIVE_PREFIX, index);
				p = vod_copy(p, conf->bootstrap_file_name_prefix.data, conf->bootstrap_file_name_prefix.len);
				p = manifest_utils_append_tracks_spec(p, tracks, MEDIA_TYPE_COUNT, media_set->has_multi_sequences);
				p = vod_copy(p, HDS_BOOTSTRAP_LIVE_SUFFIX, sizeof(HDS_BOOTSTRAP_LIVE_SUFFIX) - 1);
				break;
			}

			index++;
		}

		muxed_tracks = 1;
	}

	if (drm_enabled)
	{
		muxed_tracks = initial_muxed_tracks;
		index = 0;
		for (adaptation_set = adaptation_sets.first; adaptation_set < adaptation_sets.last; adaptation_set++)
		{
			for (cur_track_ptr = adaptation_set->first; cur_track_ptr < adaptation_set->last; cur_track_ptr += muxed_tracks)
			{
				if (cur_track_ptr[0] != NULL)
				{
					cur_sequence = cur_track_ptr[0]->file_info.source->sequence;
				}
				else
				{
					cur_sequence = cur_track_ptr[1]->file_info.source->sequence;
				}

				drm_metadata = &((drm_info_t*)cur_sequence->drm_info)->pssh_array.first->data;

				p = vod_sprintf(p, HDS_DRM_ADDITIONAL_HEADER_PREFIX, index);
				p = vod_copy(p, drm_metadata->data, drm_metadata->len);
				p = vod_copy(p, HDS_DRM_ADDITIONAL_HEADER_SUFFIX, sizeof(HDS_DRM_ADDITIONAL_HEADER_SUFFIX) - 1);

				index++;
			}

			muxed_tracks = 1;
		}
	}

	// media tags
	muxed_tracks = initial_muxed_tracks;
	index = 0;
	for (adaptation_set = adaptation_sets.first; adaptation_set < adaptation_sets.last; adaptation_set++)
	{
		for (cur_track_ptr = adaptation_set->first; cur_track_ptr < adaptation_set->last; cur_track_ptr += muxed_tracks)
		{
			// get the tracks
			if (muxed_tracks == 1)
			{
				if (cur_track_ptr[0]->media_info.media_type == MEDIA_TYPE_VIDEO)
				{
					tracks_array[MEDIA_TYPE_VIDEO] = cur_track_ptr[0];
					tracks_array[MEDIA_TYPE_AUDIO] = NULL;
				}
				else
				{
					tracks_array[MEDIA_TYPE_VIDEO] = NULL;
					tracks_array[MEDIA_TYPE_AUDIO] = cur_track_ptr[0];
				}
				tracks = tracks_array;
			}
			else
			{
				tracks = cur_track_ptr;
			}

			if (tracks[MEDIA_TYPE_VIDEO] != NULL)
			{
				bitrate = tracks[MEDIA_TYPE_VIDEO]->media_info.bitrate;
				if (tracks[MEDIA_TYPE_AUDIO] != NULL)
				{
					bitrate += tracks[MEDIA_TYPE_AUDIO]->media_info.bitrate;
				}

				p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_VIDEO,
					bitrate / 1000,
					(uint32_t)tracks[MEDIA_TYPE_VIDEO]->media_info.u.video.width,
					(uint32_t)tracks[MEDIA_TYPE_VIDEO]->media_info.u.video.height);
			}
			else
			{
				bitrate = tracks[MEDIA_TYPE_AUDIO]->media_info.bitrate;
				if (adaptation_sets.multi_audio && adaptation_set > adaptation_sets.first)
				{
					p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_AUDIO_LANG,
						bitrate / 1000, 
						&tracks[MEDIA_TYPE_AUDIO]->media_info.tags.label,
						&tracks[MEDIA_TYPE_AUDIO]->media_info.tags.lang_str);
				}
				else
				{
					p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_AUDIO,
						bitrate / 1000);
				}
			}

			// url
			p = vod_copy(p, conf->fragment_file_name_prefix.data, conf->fragment_file_name_prefix.len);
			p = manifest_utils_append_tracks_spec(p, tracks, MEDIA_TYPE_COUNT, media_set->has_multi_sequences);
			*p++ = '-';

			if (drm_enabled)
			{
				p = vod_sprintf(p, HDS_MEDIA_HEADER_SUFFIX_DRM, index, index);
			}
			else
			{
				p = vod_sprintf(p, HDS_MEDIA_HEADER_SUFFIX, index);
			}

			p = hds_amf0_write_base64_metadata(p, temp_buffer, media_set, tracks);

			p = vod_copy(p, HDS_MEDIA_FOOTER, sizeof(HDS_MEDIA_FOOTER) - 1);

			index++;
		}

		muxed_tracks = 1;
	}

	// manifest footer
	p = vod_copy(p, HDS_MANIFEST_FOOTER, sizeof(HDS_MANIFEST_FOOTER) - 1);

	result->len = p - result->data;
	
	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"hds_packager_build_manifest: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	vod_free(request_context->pool, temp_buffer);

	return VOD_OK;
}
