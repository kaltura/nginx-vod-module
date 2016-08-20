#include "mp4_parser_base.h"
#include "../read_stream.h"

vod_status_t 
mp4_parser_parse_atoms(
	request_context_t* request_context, 
	const u_char* buffer, 
	uint64_t buffer_size, 
	bool_t validate_full_atom, 
	parse_atoms_callback_t callback, 
	void* context)
{
	const u_char* cur_pos = buffer;
	const u_char* end_pos = buffer + buffer_size;
	bool_t atom_size_overflow = FALSE;
	uint64_t atom_size;
	atom_info_t atom_info;
	vod_status_t rc;
	
	while (cur_pos + ATOM_HEADER_SIZE <= end_pos)
	{
		read_be32(cur_pos, atom_size);
		read_le32(cur_pos, atom_info.name);
		
		vod_log_debug3(VOD_LOG_DEBUG_LEVEL, request_context->log, 0, 
			"mp4_parser_parse_atoms: atom name=%*s, size=%uL", (size_t)sizeof(atom_info.name), (char*)&atom_info.name, atom_size);
		
		if (atom_size == 1)
		{
			// atom_size == 1 => atom uses 64 bit size
			if (cur_pos + sizeof(uint64_t) > end_pos)
			{
				if (!validate_full_atom)
				{
					return VOD_OK;
				}

				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_parse_atoms: atom size is 1 but there is not enough room for the 64 bit size");
				return VOD_BAD_DATA;
			}
			
			read_be64(cur_pos, atom_size);
			atom_info.header_size = ATOM_HEADER64_SIZE;
		}
		else
		{
			atom_info.header_size = ATOM_HEADER_SIZE;
			if (atom_size == 0)
			{
				// atom_size == 0 => atom extends till the end of the buffer
				atom_size = (end_pos - cur_pos) + atom_info.header_size;
			}
		}
		
		if (atom_size < atom_info.header_size)
		{
			if (validate_full_atom)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_parse_atoms: atom size %uL is less than the atom header size %ud", atom_size, atom_info.header_size);
			}
			return VOD_BAD_DATA;
		}
		
		atom_size -= atom_info.header_size;
		if (atom_size > (uint64_t)(end_pos - cur_pos))
		{
			if (validate_full_atom)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_parse_atoms: atom size %uL overflows the input stream size %uL", atom_size, (uint64_t)(end_pos - cur_pos));
				return VOD_BAD_DATA;
			}

			atom_size_overflow = TRUE;
		}
		
		atom_info.ptr = cur_pos;
		atom_info.size = atom_size;
		rc = callback(context, &atom_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
		
		if (atom_size_overflow)
		{
			vod_log_debug2(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"mp4_parser_parse_atoms: atom size %uL overflows the input stream size %uL", atom_size, (uint64_t)(end_pos - cur_pos));
			return VOD_BAD_DATA;
		}
		cur_pos += atom_size;
	}
	
	return VOD_OK;
}

vod_status_t
mp4_parser_save_relevant_atoms_callback(void* ctx, atom_info_t* atom_info)
{
	save_relevant_atoms_context_t* context = (save_relevant_atoms_context_t*)ctx;
	save_relevant_atoms_context_t child_context;
	const relevant_atom_t* cur_atom;
	vod_status_t rc;

	for (cur_atom = context->relevant_atoms; cur_atom->atom_name != ATOM_NAME_NULL; cur_atom++)
	{
		if (cur_atom->atom_name != atom_info->name)
		{
			continue;
		}

		if (cur_atom->relevant_children != NULL)
		{
			child_context.relevant_atoms = cur_atom->relevant_children;
			child_context.result = context->result;
			child_context.request_context = context->request_context;
			rc = mp4_parser_parse_atoms(
				context->request_context,
				atom_info->ptr,
				atom_info->size,
				TRUE,
				&mp4_parser_save_relevant_atoms_callback,
				&child_context);
			if (rc != VOD_OK)
			{
				return rc;
			}
			continue;
		}

		*(atom_info_t*)(((u_char*)context->result) + cur_atom->atom_info_offset) = *atom_info;
	}
	return VOD_OK;
}

vod_status_t
mp4_parser_validate_stts_data(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries)
{
	const stts_atom_t* atom = (const stts_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stts_data: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	*entries = parse_be32(atom->entries);
	if (*entries >= (INT_MAX - sizeof(*atom)) / sizeof(stts_entry_t))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stts_data: number of entries %uD too big", *entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + *entries * sizeof(stts_entry_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stts_data: atom size %uL too small to hold %uD entries", atom_info->size, *entries);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
mp4_parser_validate_stss_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries)
{
	const stss_atom_t* atom = (const stss_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stss_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	*entries = parse_be32(atom->entries);
	if (*entries >= (INT_MAX - sizeof(*atom)) / sizeof(uint32_t))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stss_atom: number of entries %uD too big", *entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + *entries * sizeof(uint32_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stss_atom: atom size %uL too small to hold %uD entries", atom_info->size, *entries);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
mp4_parser_validate_ctts_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries)
{
	const ctts_atom_t* atom = (const ctts_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_ctts_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	*entries = parse_be32(atom->entries);
	if (*entries <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_ctts_atom: zero entries");
		return VOD_BAD_DATA;
	}

	if (*entries >= (INT_MAX - sizeof(*atom)) / sizeof(ctts_entry_t))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_ctts_atom: number of entries %uD too big", *entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + *entries * sizeof(ctts_entry_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_ctts_atom: atom size %uL too small to hold %uD entries", atom_info->size, *entries);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
mp4_parser_validate_stsc_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t* entries)
{
	const stsc_atom_t* atom = (const stsc_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsc_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	*entries = parse_be32(atom->entries);
	if (*entries <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsc_atom: zero entries");
		return VOD_BAD_DATA;
	}

	if (*entries >= (INT_MAX - sizeof(*atom)) / sizeof(stsc_entry_t))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsc_atom: number of entries %uD too big", *entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + *entries * sizeof(stsc_entry_t))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsc_atom: atom size %uL too small to hold %uD entries", atom_info->size, *entries);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
mp4_parser_validate_stsz_atom(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t last_frame,
	uint32_t* uniform_size,
	uint32_t* field_size,
	uint32_t* entries)
{
	const stsz_atom_t* atom = (const stsz_atom_t*)atom_info->ptr;
	const stz2_atom_t* atom2 = (const stz2_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsz_atom: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	if (atom_info->name == ATOM_NAME_STZ2)
	{
		*field_size = atom2->field_size[0];
		if (*field_size == 0)			// protect against division by zero
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"mp4_parser_validate_stsz_atom: field size is zero");
			return VOD_BAD_DATA;
		}
		*uniform_size = 0;
	}
	else
	{
		*uniform_size = parse_be32(atom->uniform_size);
		if (*uniform_size != 0)
		{
			if (*uniform_size > MAX_FRAME_SIZE)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"mp4_parser_validate_stsz_atom: uniform size %uD is too big", *uniform_size);
				return VOD_BAD_DATA;
			}

			*entries = parse_be32(atom->entries);

			return VOD_OK;
		}
		*field_size = 32;
	}

	*entries = parse_be32(atom->entries);
	if (*entries < last_frame)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsz_atom: number of entries %uD smaller than last frame %uD", *entries, last_frame);
		return VOD_BAD_DATA;
	}

	if (*entries >= INT_MAX / *field_size)			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsz_atom: number of entries %uD too big for size %ud bits", *entries, *field_size);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + vod_div_ceil(*entries * *field_size, 8))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stsz_atom: atom size %uL too small to hold %uD entries of %ud bits", atom_info->size, *entries, *field_size);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
mp4_parser_validate_stco_data(
	request_context_t* request_context,
	atom_info_t* atom_info,
	uint32_t last_chunk_index,
	uint32_t* entries, 
	uint32_t* entry_size)
{
	const stco_atom_t* atom = (const stco_atom_t*)atom_info->ptr;

	if (atom_info->size < sizeof(*atom))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stco_data: atom size %uL too small", atom_info->size);
		return VOD_BAD_DATA;
	}

	*entries = parse_be32(atom->entries);
	if (*entries < last_chunk_index)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stco_data: number of entries %uD smaller than last chunk %uD", *entries, last_chunk_index);
		return VOD_BAD_DATA;
	}

	if (atom_info->name == ATOM_NAME_CO64)
	{
		*entry_size = sizeof(uint64_t);
	}
	else
	{
		*entry_size = sizeof(uint32_t);
	}

	if (*entries >= (INT_MAX - sizeof(*atom)) / (*entry_size))			// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stco_data: number of entries %uD too big", *entries);
		return VOD_BAD_DATA;
	}

	if (atom_info->size < sizeof(*atom) + (*entries) * (*entry_size))
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_parser_validate_stco_data: atom size %uL too small to hold %uD entries", atom_info->size, *entries);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

void
mp4_parser_stts_iterator_init(
	stts_iterator_state_t* iterator, 
	media_parse_params_t* parse_params,
	stts_entry_t* first_entry, 
	uint32_t entries)
{
	iterator->cur_entry = first_entry;
	iterator->last_entry = first_entry + entries;
	iterator->sample_count = parse_be32(first_entry->count);
	iterator->frame_index = 0;
	iterator->accum_duration = 0;
}

bool_t
mp4_parser_stts_iterator(
	stts_iterator_state_t* iterator, 
	uint64_t offset)
{
	stts_entry_t* last_entry;
	stts_entry_t* cur_entry;
	uint32_t sample_duration;
	uint32_t sample_count;
	uint32_t skip_count;
	uint32_t frame_count = 0;
	uint64_t next_accum_duration;
	uint64_t accum_duration;

	cur_entry = iterator->cur_entry;
	last_entry = iterator->last_entry;
	accum_duration = iterator->accum_duration;
	sample_count = iterator->sample_count;
	sample_duration = parse_be32(cur_entry->duration);
	next_accum_duration = accum_duration + sample_duration * sample_count;

	for (;;)
	{
		if (offset != ULLONG_MAX &&
			sample_duration > 0 &&
			offset < next_accum_duration)		// Note: need to add sample_duration - 1 to offset, if changing skip_count calculation below to use div_ceil
		{
			break;
		}

		frame_count += sample_count;
		accum_duration = next_accum_duration;

		// parse the next sample
		cur_entry++;
		if (cur_entry >= last_entry)
		{
			iterator->cur_entry = cur_entry;
			iterator->sample_count = 0;
			iterator->frame_index += frame_count;
			iterator->accum_duration = accum_duration;
			return FALSE;
		}

		sample_duration = parse_be32(cur_entry->duration);
		sample_count = parse_be32(cur_entry->count);
		next_accum_duration = accum_duration + sample_duration * sample_count;
	}

	// Note: the below was done to match nginx mp4, may be better to do 
	// vod_div_ceil(offset - accum_duration, sample_duration);
	skip_count = (offset - accum_duration) / sample_duration;
	iterator->cur_entry = cur_entry;
	iterator->sample_count = sample_count - skip_count;
	iterator->frame_index += frame_count + skip_count;
	iterator->accum_duration = accum_duration + skip_count * sample_duration;

	return TRUE;
}

uint32_t
mp4_parser_find_stss_entry(
	uint32_t frame_index, 
	const uint32_t* first_entry, 
	uint32_t entries)
{
	const uint32_t* cur_entry;
	uint32_t mid_value;
	int32_t left;
	int32_t right;
	int32_t mid;

	frame_index++;	// convert to 1-based

	left = 0;
	right = entries - 1;
	while (left <= right)
	{
		mid = (left + right) / 2;
		cur_entry = first_entry + mid;
		mid_value = parse_be32(cur_entry);
		if (mid_value < frame_index)
		{
			left = mid + 1;
		}
		else if (mid_value > frame_index)
		{
			right = mid - 1;
		}
		else
		{
			return mid;
		}
	}
	return left;
}

void
mp4_parser_ctts_iterator_init(
	ctts_iterator_state_t* iterator, 
	ctts_entry_t* first_entry, 
	uint32_t entries)
{
	iterator->cur_entry = first_entry;
	iterator->last_entry = first_entry + entries;
	iterator->sample_count = parse_be32(first_entry->count);
	iterator->frame_index = 0;
}

bool_t
mp4_parser_ctts_iterator(
	ctts_iterator_state_t* iterator, 
	uint32_t required_index)
{
	ctts_entry_t* last_entry;
	ctts_entry_t* cur_entry;
	uint32_t sample_count;
	uint32_t skip_count;
	uint32_t frame_index = 0;

	cur_entry = iterator->cur_entry;
	last_entry = iterator->last_entry;
	sample_count = iterator->sample_count;
	frame_index = iterator->frame_index;

	for (;;)
	{
		if (frame_index + sample_count > required_index)
		{
			break;
		}

		frame_index += sample_count;

		// parse the next sample
		cur_entry++;
		if (cur_entry >= last_entry)
		{
			return FALSE;
		}

		sample_count = parse_be32(cur_entry->count);
	}

	skip_count = required_index - frame_index;
	iterator->cur_entry = cur_entry;
	iterator->sample_count = sample_count - skip_count;
	iterator->frame_index = required_index;

	return TRUE;
}

vod_status_t
mp4_parser_stsc_iterator_init(
	stsc_iterator_state_t* iterator,
	request_context_t* request_context,
	stsc_entry_t* first_entry,
	uint32_t entries, 
	uint32_t chunks)
{
	iterator->request_context = request_context;
	iterator->cur_entry = first_entry;
	iterator->last_entry = first_entry + entries;
	iterator->frame_index = 0;
	iterator->chunks = chunks;

	iterator->cur_chunk = parse_be32(first_entry->first_chunk);
	if (iterator->cur_chunk < 1)
	{
		vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
			"mp4_parser_stsc_iterator_init: chunk index is zero");
		return VOD_BAD_DATA;
	}
	
	iterator->samples_per_chunk = parse_be32(first_entry->samples_per_chunk);
	if (iterator->samples_per_chunk == 0)
	{
		vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
			"mp4_parser_stsc_iterator_init: samples per chunk is zero");
		return VOD_BAD_DATA;
	}
	
	iterator->sample_desc = parse_be32(first_entry->sample_desc);

	return VOD_OK;
}

vod_status_t
mp4_parser_stsc_iterator(
	stsc_iterator_state_t* iterator, 
	uint32_t required_index, 
	uint32_t* target_chunk, 
	uint32_t* sample_count,
	uint32_t* next_chunk_out,
	uint32_t* prev_samples)
{
	stsc_entry_t* last_entry = iterator->last_entry;
	stsc_entry_t* cur_entry = iterator->cur_entry;
	uint32_t frame_index = iterator->frame_index;
	uint32_t cur_chunk = iterator->cur_chunk;
	uint32_t samples_per_chunk = iterator->samples_per_chunk;
	uint32_t sample_desc = iterator->sample_desc;
	uint32_t cur_entry_samples;
	uint32_t next_chunk;

	*prev_samples = 0;

	for (; cur_entry + 1 < last_entry; cur_entry++, frame_index += cur_entry_samples)
	{
		next_chunk = parse_be32(cur_entry[1].first_chunk);
		if (next_chunk <= cur_chunk)
		{
			vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
				"mp4_parser_stsc_iterator: chunk index %uD is smaller than the previous index %uD (1)", next_chunk, cur_chunk);
			return VOD_BAD_DATA;
		}

		if (next_chunk - cur_chunk > (UINT_MAX - frame_index) / samples_per_chunk)		// integer overflow protection
		{
			vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
				"mp4_parser_stsc_iterator: chunk index %uD is too big for previous index %uD and samples per chunk %uD", next_chunk, cur_chunk, samples_per_chunk);
			return VOD_BAD_DATA;
		}

		cur_entry_samples = (next_chunk - cur_chunk) * samples_per_chunk;

		if (frame_index + cur_entry_samples > required_index)
		{
			goto found;
		}

		cur_chunk = next_chunk;
		*prev_samples = samples_per_chunk;
		samples_per_chunk = parse_be32(cur_entry[1].samples_per_chunk);
		if (samples_per_chunk == 0)
		{
			vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
				"mp4_parser_stsc_iterator: samples per chunk is zero");
			return VOD_BAD_DATA;
		}
		sample_desc = parse_be32(cur_entry[1].sample_desc);
	}

	next_chunk = iterator->chunks + 1;
	if (next_chunk < cur_chunk)
	{
		vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
			"mp4_parser_stsc_iterator: chunk index %uD is smaller than the previous index %uD (1)", next_chunk, cur_chunk);
		return VOD_BAD_DATA;
	}

	if (next_chunk - cur_chunk > (UINT_MAX - frame_index) / samples_per_chunk)		// integer overflow protection
	{
		vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
			"mp4_parser_stsc_iterator: chunk index %uD is too big for previous index %uD and samples per chunk %uD", next_chunk, cur_chunk, samples_per_chunk);
		return VOD_BAD_DATA;
	}

	cur_entry_samples = (next_chunk - cur_chunk) * samples_per_chunk;
	if (frame_index + cur_entry_samples < required_index)
	{
		vod_log_error(VOD_LOG_ERR, iterator->request_context->log, 0,
			"mp4_parser_stsc_iterator: required index %uD exceeds stsc indexes %uD", required_index, frame_index + cur_entry_samples);
		return VOD_BAD_DATA;
	}
	
found:

	iterator->cur_entry = cur_entry;
	iterator->cur_chunk = cur_chunk;
	iterator->frame_index = frame_index;
	iterator->samples_per_chunk = samples_per_chunk;
	iterator->sample_desc = sample_desc;
	
	*target_chunk = cur_chunk - 1;
	*target_chunk += (required_index - frame_index) / samples_per_chunk;
	*sample_count = (required_index - frame_index) % samples_per_chunk;
	*next_chunk_out = next_chunk;

	return VOD_OK;
}
