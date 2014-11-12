#include "hds_manifest.h"
#include "hds_fragment.h"
#include "hds_amf0_encoder.h"

// manifest constants
#define HDS_MANIFEST_HEADER								\
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"		\
	"<manifest\n"										\
	"  xmlns=\"http://ns.adobe.com/f4m/1.0\">\n"		\
	"  <id>%V</id>\n"									\
	"  <duration>%uD.%uD</duration>\n"					\
	"  <streamType>recorded</streamType>\n"

#define HDS_BOOTSTRAP_HEADER							\
	"  <bootstrapInfo\n"								\
	"    profile=\"named\"\n"							\
	"    id=\"bootstrap%uD\">"

#define HDS_BOOTSTRAP_FOOTER							\
	"</bootstrapInfo>\n"

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

#define HDS_MEDIA_HEADER_SUFFIX							\
	"\"\n"												\
	"    bootstrapInfoId=\"bootstrap%uD\">\n"			\
	"    <metadata>"


#define HDS_MEDIA_FOOTER								\
	"</metadata>\n"										\
	"  </media>\n"

#define HDS_MANIFEST_FOOTER								\
	"</manifest>\n"

#define AFRT_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(afrt_atom_t) + sizeof(afrt_entry_t) * 3 + sizeof(u_char))
#define ASRT_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(asrt_atom_t) + sizeof(asrt_entry_t))
#define ABST_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(abst_atom_t) + ASRT_ATOM_SIZE + sizeof(u_char) + AFRT_ATOM_SIZE)

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

static u_char*
hds_write_abst_atom(u_char* p, uint64_t duration, uint32_t timescale, uint32_t segment_duration)
{
	uint64_t duration_millis = rescale_time(duration, timescale, HDS_TIMESCALE);
	uint32_t segment_count;
	uint64_t fragment_timestamp;
	uint32_t fragment_duration;
	size_t afrt_atom_size = AFRT_ATOM_SIZE;
	size_t asrt_atom_size = ASRT_ATOM_SIZE;
	size_t abst_atom_size = ABST_ATOM_SIZE;

	segment_count = (duration_millis + 999) / segment_duration;
	if (segment_count <= 1)
	{
		segment_count = 1;
		afrt_atom_size -= sizeof(afrt_entry_t);
		abst_atom_size -= sizeof(afrt_entry_t);
	}

	// abst
	write_atom_header(p, abst_atom_size, 'a', 'b', 's', 't');
	write_dword(p, 0);					// version + flags
	write_dword(p, 1);					// bootstrap info version
	*p++ = 0;							// profile, live, update
	write_dword(p, HDS_TIMESCALE);		// timescale
	write_qword(p, duration_millis);	// current media time
	write_qword(p, 0LL);				// smpte offset
	*p++ = 0;							// movie identifier
	*p++ = 0;							// server entries
	*p++ = 0;							// quality entries
	*p++ = 0;							// drm data
	*p++ = 0;							// metadata
	*p++ = 1;							// segment run table count

	// abst.asrt
	write_atom_header(p, asrt_atom_size, 'a', 's', 'r', 't');
	write_dword(p, 0);					// version + flags
	*p++ = 0;							// quality entries
	write_dword(p, 1);					// segment run entries
	// entry #1
	write_dword(p, 1);					// first segment
	write_dword(p, segment_count);		// fragments per segment

	// abst
	*p++ = 1;							// fragment run table count

	// abst.afrt
	write_atom_header(p, afrt_atom_size, 'a', 'f', 'r', 't');
	write_dword(p, 0);					// version + flags
	write_dword(p, HDS_TIMESCALE);		// timescale
	*p++ = 0;							// quality entries
	if (segment_count > 1)
	{
		write_dword(p, 3);				// fragment run entries
		// entry #1
		write_dword(p, 1);				// first fragment
		write_qword(p, 0LL);			// first fragment timestamp
		write_dword(p, segment_duration);	// fragment duration
		// entry #2
		fragment_timestamp = ((uint64_t)segment_count - 1) * segment_duration;
		fragment_duration = duration_millis - fragment_timestamp;
		write_dword(p, segment_count);		// first fragment
		write_qword(p, fragment_timestamp);	// first fragment timestamp
		write_dword(p, fragment_duration);	// fragment duration
	}
	else
	{
		write_dword(p, 2);				// fragment run entries
		// entry #1
		write_dword(p, 1);				// first fragment
		write_qword(p, 0LL);			// first fragment timestamp
		write_dword(p, duration_millis);	// fragment duration
	}
	// entry #3
	write_dword(p, 0);					// first fragment
	write_qword(p, 0LL);				// first fragment timestamp
	write_dword(p, 0);					// fragment duration
	*p++ = 0;							// discontinuity indicator (0 = end of presentation)

	return p;
}

static u_char*
hds_write_base64_abst_atom(u_char* p, uint64_t duration, uint32_t timescale, uint32_t segment_duration)
{
	vod_str_t binary;
	vod_str_t base64;
	u_char abst_atom[ABST_ATOM_SIZE];

	binary.data = abst_atom;
	binary.len = hds_write_abst_atom(binary.data, duration, timescale, segment_duration) - binary.data;

	base64.data = p;

	vod_encode_base64(&base64, &binary);

	return p + base64.len;
}

vod_status_t
hds_packager_build_manifest(
	request_context_t* request_context,
	hds_manifest_config_t* conf,
	vod_str_t* manifest_id,
	uint32_t segment_duration,
	bool_t include_file_index,
	mpeg_metadata_t* mpeg_metadata,
	vod_str_t* result)
{
	WALK_STREAMS_BY_FILES_VARS(cur_file_streams);
	mpeg_stream_metadata_t* stream;
	uint64_t duration;
	uint32_t timescale;
	uint32_t bitrate;
	uint32_t index;
	size_t result_size;
	u_char* temp_metadata_buffer;
	u_char* p;

	// calculate the result size
	result_size = 
		sizeof(HDS_MANIFEST_HEADER) - 1 + 2 * VOD_INT32_LEN + manifest_id->len + 
		sizeof(HDS_MANIFEST_FOOTER);

	WALK_STREAMS_BY_FILES_START(cur_file_streams, mpeg_metadata)

		result_size += 
			sizeof(HDS_BOOTSTRAP_HEADER) - 1 + VOD_INT32_LEN + 
				vod_base64_encoded_length(ABST_ATOM_SIZE) +
			sizeof(HDS_BOOTSTRAP_FOOTER) - 1;
		
		result_size += 
			sizeof(HDS_MEDIA_HEADER_PREFIX_VIDEO) - 1 + 3 * VOD_INT32_LEN +
			conf->fragment_file_name_prefix.len + sizeof("-f-v-a-") - 1 + 3 * VOD_INT32_LEN + 
			sizeof(HDS_MEDIA_HEADER_SUFFIX) - 1 + VOD_INT32_LEN + 
				vod_base64_encoded_length(amf0_max_total_size) +
			sizeof(HDS_MEDIA_FOOTER) - 1;

	WALK_STREAMS_BY_FILES_END(cur_file_streams, mpeg_metadata)

	// allocate the buffers
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	temp_metadata_buffer = vod_alloc(request_context->pool, amf0_max_total_size);
	if (temp_metadata_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	// print the manifest header
	p = vod_sprintf(result->data, HDS_MANIFEST_HEADER,
		manifest_id,
		(uint32_t)(mpeg_metadata->duration_millis / 1000),
		(uint32_t)(mpeg_metadata->duration_millis % 1000));

	// bootstrap tags
	index = 0;
	WALK_STREAMS_BY_FILES_START(cur_file_streams, mpeg_metadata)

		p = vod_sprintf(p, HDS_BOOTSTRAP_HEADER,
			index++);

		hds_get_max_duration(cur_file_streams, &duration, &timescale);

		p = hds_write_base64_abst_atom(p, duration, timescale, segment_duration);

		p = vod_copy(p, HDS_BOOTSTRAP_FOOTER, sizeof(HDS_BOOTSTRAP_FOOTER) - 1);

	WALK_STREAMS_BY_FILES_END(cur_file_streams, mpeg_metadata)

	// media tags
	index = 0;
	WALK_STREAMS_BY_FILES_START(cur_file_streams, mpeg_metadata)

		if (cur_file_streams[MEDIA_TYPE_VIDEO] != NULL)
		{
			stream = cur_file_streams[MEDIA_TYPE_VIDEO];
			bitrate = stream->media_info.bitrate;
			if (cur_file_streams[MEDIA_TYPE_AUDIO] != NULL)
			{
				bitrate += cur_file_streams[MEDIA_TYPE_AUDIO]->media_info.bitrate;
			}

			p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_VIDEO,
				bitrate / 1000,
				(uint32_t)stream->media_info.u.video.width,
				(uint32_t)stream->media_info.u.video.height);
		}
		else
		{
			stream = cur_file_streams[MEDIA_TYPE_AUDIO];
			p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_AUDIO,
				stream->media_info.bitrate / 1000);
		}

		// url
		p = vod_copy(p, conf->fragment_file_name_prefix.data, conf->fragment_file_name_prefix.len);
		if (include_file_index)
		{
			p = vod_sprintf(p, "-f%uD", stream->file_info.file_index + 1);
		}

		if (cur_file_streams[MEDIA_TYPE_VIDEO] != NULL)
		{
			p = vod_sprintf(p, "-v%uD", cur_file_streams[MEDIA_TYPE_VIDEO]->track_index + 1);
		}

		if (cur_file_streams[MEDIA_TYPE_AUDIO] != NULL)
		{
			p = vod_sprintf(p, "-a%uD", cur_file_streams[MEDIA_TYPE_AUDIO]->track_index + 1);
		}
		*p++ = '-';

		p = vod_sprintf(p, HDS_MEDIA_HEADER_SUFFIX, index++);
		
		p = hds_amf0_write_base64_metadata(p, temp_metadata_buffer, cur_file_streams);

		p = vod_copy(p, HDS_MEDIA_FOOTER, sizeof(HDS_MEDIA_FOOTER) - 1);

	WALK_STREAMS_BY_FILES_END(cur_file_streams, mpeg_metadata)

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

	vod_free(request_context->pool, temp_metadata_buffer);

	return VOD_OK;
}

