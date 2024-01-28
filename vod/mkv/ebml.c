#include "ebml.h"

// macros
#define log2_byte(value) ((log2_table[(value) >> 2] >> (((value) & 2) << 1)) & 0xf)

// constants
#define EBML_ID_HEADER             (0x1A45DFA3)

#define EBML_ID_EBMLVERSION        (0x4286)
#define EBML_ID_EBMLREADVERSION    (0x42F7)
#define EBML_ID_EBMLMAXIDLENGTH    (0x42F2)
#define EBML_ID_EBMLMAXSIZELENGTH  (0x42F3)
#define EBML_ID_DOCTYPE            (0x4282)
#define EBML_ID_DOCTYPEVERSION     (0x4287)
#define EBML_ID_DOCTYPEREADVERSION (0x4285)

#define EBML_VERSION (1)

// globals
static const uint8_t log2_table[] = {
	0x10, 0x22, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44,
	0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
};

static const uint64_t ebml_max_sizes[] = {
	0,			// none
	8,			// uint
	8,			// float
	0x1000000,	// string - 16MB
	0x10000000, // binary - 256MB
	0			// master
};

static ebml_spec_t ebml_header[] = {
	{ EBML_ID_EBMLREADVERSION,		EBML_UINT,		offsetof(ebml_header_t, version),			NULL },
	{ EBML_ID_EBMLMAXSIZELENGTH,	EBML_UINT,		offsetof(ebml_header_t, max_size),			NULL },
	{ EBML_ID_EBMLMAXIDLENGTH,		EBML_UINT,		offsetof(ebml_header_t, id_length),			NULL },
	{ EBML_ID_DOCTYPE,				EBML_STRING,	offsetof(ebml_header_t, doctype),			NULL },
	{ EBML_ID_DOCTYPEREADVERSION,	EBML_UINT,		offsetof(ebml_header_t, doctype_version),	NULL },
	{ 0, EBML_NONE, 0, NULL }
};

static ebml_spec_t ebml_syntax[] = {
	{ EBML_ID_HEADER, EBML_MASTER, 0, ebml_header },
	{ 0, EBML_NONE, 0, NULL }
};

vod_status_t
ebml_read_num(ebml_context_t* context, uint64_t* result, size_t max_size, int remove_first_bit)
{
	uint64_t value;
	uint8_t first_byte;
	size_t num_size;
	int log2_first_byte;
	int bytes_to_read;

	if (context->cur_pos >= context->end_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_read_num: stream overflow (1)");
		return VOD_BAD_DATA;
	}

	first_byte = *context->cur_pos++;
	log2_first_byte = log2_byte(first_byte);

	num_size = 8 - log2_first_byte;
	if (num_size > max_size)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_read_num: number size %uz greater than the limit %uz", num_size, max_size);
		return VOD_BAD_DATA;
	}

	bytes_to_read = num_size - 1;
	if (bytes_to_read > context->end_pos - context->cur_pos)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_read_num: stream overflow (2)");
		return VOD_BAD_DATA;
	}

	value = first_byte & ~(remove_first_bit << log2_first_byte);
	for (; bytes_to_read > 0; bytes_to_read--)
	{
		value = (value << 8) | (*context->cur_pos++);
	}

	*result = value;
	return num_size;
}

static vod_status_t
ebml_read_size(ebml_context_t* context, uint64_t* result, bool_t truncate)
{
	vod_status_t rc;
	uint64_t left;

	rc = ebml_read_num(context, result, 8, 1);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"ebml_read_size: ebml_read_num failed %i", rc);
		return rc;
	}

	left = context->end_pos - context->cur_pos;
	if (is_unknown_size(*result, rc))
	{
		*result = left;
		return VOD_OK;
	}

	if (*result <= left)
	{
		return VOD_OK;
	}

	if (truncate)
	{
		*result = left;
		return VOD_OK;
	}

	vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
		"ebml_read_size: size %uL greater than the remaining stream bytes %uL",
		*result, left);
	return VOD_BAD_DATA;
}

static vod_status_t
ebml_read_uint(ebml_context_t* context, size_t size, uint64_t* result)
{
	const u_char* src = context->cur_pos;
	uint64_t value;

	value = 0;
	for (; size > 0; size--)
	{
		value = (value << 8) | (*src++);
	}

	*result = value;
	return VOD_OK;
}

static vod_status_t
ebml_read_float(ebml_context_t* context, size_t size, double *result)
{
	const u_char* src;
	u_char* dest;
	float f;

	switch (size)
	{
	case 0:
		*result = 0;
		break;

	case 4:
		src = context->cur_pos;
		dest = (u_char*)&f + 3;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*result = f;
		break;

	case 8:
		src = context->cur_pos;
		dest = (u_char*)result + 7;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		*dest-- = *src++;
		break;

	default:
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_read_float: invalid float size %uz", size);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

static vod_status_t
ebml_parse_element(ebml_context_t* context, ebml_spec_t* spec, void* dest)
{
	ebml_parser_t parser;
	ebml_context_t next_context;
	uint64_t max_size;
	uint64_t size;
	void* cur_dest;
	vod_status_t rc;
	ebml_type_t type;

	// size
	rc = ebml_read_size(context, &size, spec->type & EBML_TRUNCATE_SIZE);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"ebml_parse_element: ebml_read_size failed, id=0x%uxD", spec->id);
		return rc;
	}

	if (spec->type == EBML_NONE)
	{
		context->cur_pos += size;
		return VOD_OK;
	}

	type = spec->type & ~EBML_TRUNCATE_SIZE;
	max_size = ebml_max_sizes[type];
	if (max_size && size > max_size)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_parse_element: invalid size %uz for element 0x%uxD", size, spec->id);
		return VOD_BAD_DATA;
	}

	cur_dest = (u_char*)dest + spec->offset;

	switch (type)
	{
	case EBML_UINT:
		rc = ebml_read_uint(context, size, cur_dest);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"ebml_parse_element: ebml_read_uint failed, id=0x%uxD", spec->id);
			return rc;
		}
		break;

	case EBML_FLOAT:
		rc = ebml_read_float(context, size, cur_dest);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"ebml_parse_element: ebml_read_float failed, id=0x%uxD", spec->id);
			return rc;
		}
		break;

	case EBML_STRING:
	case EBML_BINARY:
		((vod_str_t*)cur_dest)->data = (u_char*)context->cur_pos;
		((vod_str_t*)cur_dest)->len = size;
		break;

	case EBML_MASTER:
		next_context = *context;
		next_context.cur_pos += size;

		context->end_pos = next_context.cur_pos;
		rc = ebml_parse_master(context, spec->child, cur_dest);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"ebml_parse_element: ebml_parse_master failed, id=0x%uxD", spec->id);
			return rc;
		}
		*context = next_context;
		return VOD_OK;

	case EBML_CUSTOM:
		next_context = *context;
		next_context.cur_pos += size;

		context->end_pos = next_context.cur_pos;
		parser = spec->child;
		rc = parser(context, spec, cur_dest);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"ebml_parse_element: parser failed, id=0x%uxD", spec->id);
			return rc;
		}
		*context = next_context;
		return VOD_OK;

	default:;
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_parse_element: unsupported type, id=0x%uxD", spec->id);
	}

	context->cur_pos += size;
	return VOD_OK;
}

vod_status_t
ebml_parse_single(ebml_context_t* context, ebml_spec_t* spec, void* dest)
{
	ebml_spec_t* cur_spec;
	vod_status_t rc;
	uint64_t id;

	rc = ebml_read_id(context, &id);
	if (rc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"ebml_parse_single: ebml_read_id failed, rc=%i", rc);
		return rc;
	}

	for (cur_spec = spec; cur_spec->type != EBML_NONE; cur_spec++)
	{
		if (cur_spec->id == id)
		{
			break;
		}
	}

	rc = ebml_parse_element(context, cur_spec, dest);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"ebml_parse_single: ebml_parse_element failed, id=0x%uxL", id);
		return rc;
	}

	return VOD_OK;
}

vod_status_t
ebml_parse_master(ebml_context_t* context, ebml_spec_t* spec, void* dest)
{
	vod_status_t rc;

	while (context->cur_pos < context->end_pos)
	{
		rc = ebml_parse_single(context, spec, dest);
		if (rc != VOD_OK)
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
				"ebml_parse_master: ebml_parse_single failed, rc=%i", rc);
			return rc;
		}
	}

	return VOD_OK;
}

vod_status_t
ebml_parse_header(ebml_context_t* context, ebml_header_t* header)
{
	vod_status_t rc;

	vod_memzero(header, sizeof(*header));

	rc = ebml_parse_single(context, ebml_syntax, header);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"ebml_parse_header: ebml_parse_single failed, rc=%i", rc);
		return rc;
	}

	if (header->version > EBML_VERSION ||
		header->max_size > sizeof(uint64_t) ||
		header->id_length > sizeof(uint32_t) ||
		header->doctype_version > 3)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"ebml_parse_header: unsupported header, version=%uL, maxSize=%uL, idLength=%uL, docTypeVersion=%uL",
			header->version, header->max_size, header->id_length, header->doctype_version);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}
