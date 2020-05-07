#include "mp4_format.h"
#include "mp4_parser.h"
#include "mp4_clipper.h"

// constants
#define MAX_MOOV_START_READS (4)		// maximum number of attempts to find the moov atom start for non-fast-start files

// enums
enum {
	STATE_READ_MOOV_HEADER,
	STATE_READ_MOOV_DATA,
};

// typedefs
typedef struct {
	request_context_t* request_context;
	size_t max_moov_size;
	int moov_start_reads;
	int state;
	vod_str_t parts[MP4_METADATA_PART_COUNT];
} mp4_read_metadata_state_t;

static vod_status_t 
mp4_reader_identify_callback(
	void* context,
	atom_info_t* atom_info)
{
	switch (atom_info->name)
	{
	case ATOM_NAME_FTYP:
	case ATOM_NAME_MOOV:
	case ATOM_NAME_MDAT:
		*(bool_t*)context = TRUE;
		return VOD_NOT_FOUND;		// stop the iteration
	}

	return VOD_OK;
}

static vod_status_t
mp4_metadata_reader_init(
	request_context_t* request_context, 
	vod_str_t* buffer, 
	size_t max_metadata_size,
	void** ctx)
{
	mp4_read_metadata_state_t* state;
	bool_t atom_found = FALSE;

	mp4_parser_parse_atoms(
		request_context,
		buffer->data,
		buffer->len,
		FALSE,
		mp4_reader_identify_callback,
		&atom_found);
	if (!atom_found)
	{
		return VOD_NOT_FOUND;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_metadata_reader_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->moov_start_reads = MAX_MOOV_START_READS;
	state->max_moov_size = max_metadata_size;
	state->state = STATE_READ_MOOV_HEADER;
	state->parts[MP4_METADATA_PART_FTYP].len = 0;
	*ctx = state;
	return VOD_OK;
}

static vod_status_t
mp4_metadata_reader_read(
	void* ctx,
	uint64_t offset,
	vod_str_t* buffer,
	media_format_read_metadata_result_t* result)
{
	mp4_read_metadata_state_t* state = ctx;
	const u_char* ftyp_ptr;
	size_t ftyp_size;
	u_char* uncomp_buffer;
	off_t moov_offset;
	size_t moov_size;
	vod_status_t rc;

	if (state->state == STATE_READ_MOOV_DATA)
	{
		// make sure we got the whole moov atom
		moov_size = state->parts[MP4_METADATA_PART_MOOV].len;
		if (buffer->len < moov_size)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mp4_metadata_reader_read: buffer size %uz is smaller than moov size %uz", 
				buffer->len, moov_size);
			return VOD_BAD_DATA;
		}
		moov_offset = 0;

		goto done;
	}

	if (state->parts[MP4_METADATA_PART_FTYP].len == 0)
	{
		// try to find the ftyp atom
		rc = mp4_parser_get_ftyp_atom_into(
			state->request_context,
			buffer->data,
			buffer->len,
			&ftyp_ptr,
			&ftyp_size);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mp4_metadata_reader_read: mp4_parser_get_ftyp_atom_into failed %i", rc);
			return rc;
		}

		if (ftyp_size > 0 &&
			ftyp_ptr + ftyp_size <= buffer->len + buffer->data)
		{
			// got a full ftyp atom
			state->parts[MP4_METADATA_PART_FTYP].data = vod_alloc(state->request_context->pool, ftyp_size);
			if (state->parts[MP4_METADATA_PART_FTYP].data == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
					"mp4_metadata_reader_read: vod_alloc failed");
				return VOD_ALLOC_FAILED;
			}

			vod_memcpy(state->parts[MP4_METADATA_PART_FTYP].data, ftyp_ptr, ftyp_size);
			state->parts[MP4_METADATA_PART_FTYP].len = ftyp_size;
		}
		else
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"mp4_metadata_reader_read: ftyp atom not found");
		}
	}

	// get moov atom offset and size
	rc = mp4_parser_get_moov_atom_info(
		state->request_context,
		buffer->data,
		buffer->len,
		&moov_offset,
		&moov_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"mp4_metadata_reader_read: mp4_parser_get_moov_atom_info failed %i", rc);
		return rc;
	}

	if (moov_size <= 0)
	{
		// moov not found
		if ((size_t)moov_offset < buffer->len)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mp4_metadata_reader_read: moov start offset %O is smaller than the buffer size %uz",
				moov_offset, buffer->len);
			return VOD_BAD_DATA;
		}

		if (state->moov_start_reads <= 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"mp4_metadata_reader_read: exhausted all moov read attempts");
			return VOD_BAD_DATA;
		}

		state->moov_start_reads--;

		// perform another read attempt
		result->read_req.read_offset = offset + moov_offset;
		result->read_req.read_size = 0;
		result->read_req.flags = 0;

		return VOD_AGAIN;
	}

	// save the moov size
	state->parts[MP4_METADATA_PART_MOOV].len = moov_size;

	// check whether we already have the whole atom
	if (moov_offset + moov_size <= buffer->len)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"mp4_metadata_reader_read: already read the full moov atom");
		goto done;
	}

	// validate the moov size
	if (moov_size > state->max_moov_size)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_metadata_reader_read: moov size %uD exceeds the max %uz",
			moov_size, state->max_moov_size);
		return VOD_BAD_DATA;
	}

	state->state = STATE_READ_MOOV_DATA;
	result->read_req.read_offset = offset + moov_offset;
	result->read_req.read_size = moov_size;
	result->read_req.flags = 0;

	return VOD_AGAIN;

done:

	state->parts[MP4_METADATA_PART_MOOV].data = buffer->data + moov_offset;

	// uncompress the moov atom if needed
	rc = mp4_parser_uncompress_moov(
		state->request_context,
		state->parts[MP4_METADATA_PART_MOOV].data,
		moov_size,
		state->max_moov_size,
		&uncomp_buffer,
		&moov_offset,
		&moov_size);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"mp4_metadata_reader_read: mp4_parser_uncompress_moov failed %i", rc);
		return rc;
	}

	if (uncomp_buffer != NULL)
	{
		state->parts[MP4_METADATA_PART_MOOV].data = uncomp_buffer + moov_offset;
		state->parts[MP4_METADATA_PART_MOOV].len = moov_size;
	}

	result->parts = state->parts;
	result->part_count = MP4_METADATA_PART_COUNT;

	return VOD_OK;
}

media_format_t mp4_format = {
	FORMAT_ID_MP4,
	vod_string("mp4"),
	mp4_metadata_reader_init,
	mp4_metadata_reader_read,
	mp4_clipper_parse_moov,
	mp4_clipper_build_header,
	mp4_parser_parse_basic_metadata,
	mp4_parser_parse_frames,
};
