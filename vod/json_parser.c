#include "json_parser.h"
#include <ctype.h>

// constants
#define MAX_JSON_ELEMENTS (524288)
#define MAX_RECURSION_DEPTH (32)
#define FIRST_PART_COUNT (1)		// XXXXX increase this ! only for testing purpose
#define MAX_PART_SIZE (65536)

// macros
#define ASSERT_CHAR(state, ch)										\
	if (*(state)->cur_pos != ch)									\
	{																\
		vod_snprintf(state->error, state->error_size, "expected 0x%xd got 0x%xd%Z", (int)ch, (int)*(state)->cur_pos); \
		return VOD_JSON_BAD_DATA;									\
	}

#define EXPECT_CHAR(state, ch)										\
	ASSERT_CHAR(state, ch)											\
	(state)->cur_pos++;

#define EXPECT_STRING(state, str)									\
	if (vod_strncmp((state)->cur_pos, str, sizeof(str) - 1) != 0)	\
	{																\
		vod_snprintf(state->error, state->error_size, "expected %s%Z", str); \
		return VOD_JSON_BAD_DATA;									\
	}																\
	(state)->cur_pos += sizeof(str) - 1;

// typedefs
typedef struct {
	vod_pool_t* pool;
	u_char* cur_pos;
	int depth;
	u_char* error;
	size_t error_size;
} vod_json_parser_state_t;

typedef struct {
	int type;
	size_t size;
	vod_json_status_t (*parser)(vod_json_parser_state_t* state, void* result);
} vod_json_type_t;

// forward declarations
static vod_json_status_t vod_json_parse_value(vod_json_parser_state_t* state, vod_json_value_t* result);

static vod_json_status_t vod_json_parser_string(vod_json_parser_state_t* state, void* result);
static vod_json_status_t vod_json_parser_array(vod_json_parser_state_t* state, void* result);
static vod_json_status_t vod_json_parser_object(vod_json_parser_state_t* state, void* result);
static vod_json_status_t vod_json_parser_bool(vod_json_parser_state_t* state, void* result);
static vod_json_status_t vod_json_parser_frac(vod_json_parser_state_t* state, void* result);
static vod_json_status_t vod_json_parser_int(vod_json_parser_state_t* state, void* result);

// globals
static vod_json_type_t vod_json_string = {
	VOD_JSON_STRING, sizeof(vod_str_t), vod_json_parser_string
};

static vod_json_type_t vod_json_array = {
	VOD_JSON_ARRAY, sizeof(vod_json_array_t), vod_json_parser_array
};

static vod_json_type_t vod_json_object = {
	VOD_JSON_OBJECT, sizeof(vod_json_object_t), vod_json_parser_object
};

static vod_json_type_t vod_json_bool = {
	VOD_JSON_BOOL, sizeof(bool_t), vod_json_parser_bool
};

static vod_json_type_t vod_json_frac = {
	VOD_JSON_FRAC, sizeof(vod_json_fraction_t), vod_json_parser_frac
};

static vod_json_type_t vod_json_int = {
	VOD_JSON_INT, sizeof(int64_t), vod_json_parser_int
};

static vod_json_status_t
vod_json_get_value_type(vod_json_parser_state_t* state, vod_json_type_t** result)
{
	u_char* cur_pos = state->cur_pos;

	switch (*cur_pos)
	{
	case '"':
		*result = &vod_json_string;
		return VOD_JSON_OK;

	case '[':
		*result = &vod_json_array;
		return VOD_JSON_OK;

	case '{':
		*result = &vod_json_object;
		return VOD_JSON_OK;

	case 'f':
	case 't':
		*result = &vod_json_bool;
		return VOD_JSON_OK;

	default:
		break;		// handled outside the switch
	}

	if (*cur_pos == '-')
	{
		cur_pos++;
	}

	if (!isdigit(*cur_pos))
	{
		vod_snprintf(state->error, state->error_size, "expected digit got 0x%xd%Z", (int)*cur_pos);
		return VOD_JSON_BAD_DATA;
	}

	while (isdigit(*cur_pos))
	{
		cur_pos++;
	}

	if (*cur_pos == '.')
	{
		*result = &vod_json_frac;
	}
	else
	{
		*result = &vod_json_int;
	}
	return VOD_JSON_OK;
}

static void 
vod_json_skip_spaces(vod_json_parser_state_t* state)
{
	for (; *state->cur_pos && isspace(*state->cur_pos); state->cur_pos++);
}

static vod_json_status_t
vod_json_parse_string(vod_json_parser_state_t* state, vod_str_t* result)
{
	u_char c;

	state->cur_pos++;		// skip the "

	result->data = state->cur_pos;

	for (;;)
	{
		c = *state->cur_pos;
		if (!c)
		{
			break;
		}

		switch (c)
		{
		case '\\':
			state->cur_pos++;
			if (!*state->cur_pos)
			{
				vod_snprintf(state->error, state->error_size, "end of data while parsing string (1)%Z");
				return VOD_JSON_BAD_DATA;
			}
			break;

		case '"':
			result->len = state->cur_pos - result->data;
			state->cur_pos++;
			return VOD_JSON_OK;
		}

		state->cur_pos++;
	}
	vod_snprintf(state->error, state->error_size, "end of data while parsing string (2)%Z");
	return VOD_JSON_BAD_DATA;
}

static vod_json_status_t
vod_json_parse_object_key(vod_json_parser_state_t* state, vod_json_key_value_t* result)
{
	vod_uint_t hash = 0;
	u_char c;

	EXPECT_CHAR(state, '\"');

	result->key.data = state->cur_pos;

	for (;;)
	{
		c = *state->cur_pos;
		if (!c)
		{
			break;
		}

		if (c >= 'A' && c <= 'Z')
		{
			c |= 0x20;			// tolower
			*state->cur_pos = c;
		}

		switch (c)
		{
		case '\\':
			state->cur_pos++;
			if (!*state->cur_pos)
			{
				vod_snprintf(state->error, state->error_size, "end of data while parsing string (1)%Z");
				return VOD_JSON_BAD_DATA;
			}
			break;

		case '"':
			result->key.len = state->cur_pos - result->key.data;
			result->key_hash = hash;
			state->cur_pos++;
			return VOD_JSON_OK;
		}

		hash = vod_hash(hash, c);

		state->cur_pos++;
	}

	vod_snprintf(state->error, state->error_size, "end of data while parsing string (2)%Z");
	return VOD_JSON_BAD_DATA;
}

static vod_json_status_t
vod_json_parse_int(vod_json_parser_state_t* state, int64_t* result, bool_t* negative)
{
	int64_t value;

	if (*state->cur_pos == '-')
	{
		*negative = TRUE;
		state->cur_pos++;
	}
	else
	{
		*negative = FALSE;
	}

	if (!isdigit(*state->cur_pos))
	{
		vod_snprintf(state->error, state->error_size, "expected digit got 0x%xd%Z", (int)*state->cur_pos);
		return VOD_JSON_BAD_DATA;
	}

	value = 0;

	do
	{
		if (value > LLONG_MAX / 10 - 1)
		{
			vod_snprintf(state->error, state->error_size, "number value overflow (1)%Z");
			return VOD_JSON_BAD_DATA;
		}

		value = value * 10 + (*state->cur_pos - '0');
		state->cur_pos++;
	} while (isdigit(*state->cur_pos));

	*result = value;

	return VOD_JSON_OK;
}

static vod_json_status_t
vod_json_parse_fraction(vod_json_parser_state_t* state, vod_json_fraction_t* result)
{
	vod_json_status_t rc;
	int64_t value;
	uint64_t denom = 1;
	bool_t negative;

	rc = vod_json_parse_int(state, &value, &negative);
	if (rc != VOD_JSON_OK)
	{
		return rc;
	}

	if (*state->cur_pos == '.')
	{
		state->cur_pos++;

		if (!isdigit(*state->cur_pos))
		{
			vod_snprintf(state->error, state->error_size, "expected digit got 0x%xd%Z", (int)*state->cur_pos);
			return VOD_JSON_BAD_DATA;
		}

		do
		{
			if (value > LLONG_MAX / 10 - 1 || denom > ULLONG_MAX / 10)
			{
				vod_snprintf(state->error, state->error_size, "number value overflow (2)%Z");
				return VOD_JSON_BAD_DATA;
			}

			value = value * 10 + (*state->cur_pos - '0');
			denom *= 10;
			state->cur_pos++;
		} while (isdigit(*state->cur_pos));
	}

	if (negative)
	{
		value = -value;
	}

	result->num = value;
	result->denom = denom;

	return VOD_OK;
}

static vod_json_status_t
vod_json_parse_array(vod_json_parser_state_t* state, vod_json_array_t* result)
{
	vod_array_part_t* part;
	vod_json_type_t* type;
	size_t initial_part_count;
	size_t part_size;
	void* cur_item;
	vod_status_t rc;

	state->cur_pos++;		// skip the [
	vod_json_skip_spaces(state);
	if (*state->cur_pos == ']')
	{
		result->type = VOD_JSON_NULL;
		result->count = 0;
		result->part.first = NULL;
		result->part.last = NULL;
		result->part.count = 0;
		result->part.next = NULL;

		state->cur_pos++;
		return VOD_JSON_OK;
	}

	if (state->depth >= MAX_RECURSION_DEPTH)
	{
		vod_snprintf(state->error, state->error_size, "max recursion depth exceeded%Z");
		return VOD_JSON_BAD_DATA;
	}
	state->depth++;

	rc = vod_json_get_value_type(state, &type);
	if (rc != VOD_JSON_OK)
	{
		return rc;
	}

	initial_part_count = 0;

	// initialize the result and first part
	result->type = type->type;
	result->count = 0;
	part = &result->part;
	part_size = type->size * FIRST_PART_COUNT;
	cur_item = vod_alloc(state->pool, part_size);
	if (cur_item == NULL)
	{
		return VOD_JSON_ALLOC_FAILED;
	}
	part->first = cur_item;
	part->last = (u_char*)cur_item + part_size;

	for (;;)
	{
		if (result->count >= MAX_JSON_ELEMENTS)
		{
			vod_snprintf(state->error, state->error_size, "array elements count exceeds the limit%Z");
			return VOD_JSON_BAD_DATA;
		}

		if (cur_item >= part->last)
		{
			// update the part count
			part->count = result->count - initial_part_count;
			initial_part_count = result->count;

			// allocate another part
			if (part_size < (MAX_PART_SIZE - sizeof(*part)) / 2)
			{
				part_size *= 2;
			}

			part->next = vod_alloc(state->pool, sizeof(*part) + part_size);
			if (part->next == NULL)
			{
				return VOD_JSON_ALLOC_FAILED;
			}

			part = part->next;
			cur_item = part + 1;
			part->first = cur_item;
			part->last = (u_char*)cur_item + part_size;
		}

		rc = type->parser(state, cur_item);
		if (rc != VOD_JSON_OK)
		{
			return rc;
		}

		cur_item = (u_char*)cur_item + type->size;
		result->count++;

		vod_json_skip_spaces(state);
		switch (*state->cur_pos)
		{
		case ']':
			state->cur_pos++;
			goto done;

		case ',':
			state->cur_pos++;
			vod_json_skip_spaces(state);
			continue;
		}

		vod_snprintf(state->error, state->error_size, "expected , or ] while parsing array, got 0x%xd%Z", (int)*state->cur_pos);
		return VOD_JSON_BAD_DATA;
	}

done:

	part->last = cur_item;
	part->count = result->count - initial_part_count;
	part->next = NULL;

	state->depth--;
	return VOD_JSON_OK;
}

static vod_json_status_t
vod_json_parse_object(vod_json_parser_state_t* state, vod_json_object_t* result)
{
	vod_json_key_value_t* cur_item;
	vod_status_t rc;

	state->cur_pos++;		// skip the {
	vod_json_skip_spaces(state);
	if (*state->cur_pos == '}')
	{
		result->nelts = 0;
		result->size = sizeof(*cur_item);
		result->nalloc = 0;
		result->pool = state->pool;
		result->elts = NULL;

		state->cur_pos++;
		return VOD_JSON_OK;
	}

	if (state->depth >= MAX_RECURSION_DEPTH)
	{
		vod_snprintf(state->error, state->error_size, "max recursion depth exceeded%Z");
		return VOD_JSON_BAD_DATA;
	}
	state->depth++;

	rc = vod_array_init(result, state->pool, 5, sizeof(*cur_item));
	if (rc != VOD_OK)
	{
		return VOD_JSON_ALLOC_FAILED;
	}

	for (;;)
	{
		if (result->nelts >= MAX_JSON_ELEMENTS)
		{
			vod_snprintf(state->error, state->error_size, "object elements count exceeds the limit%Z");
			return VOD_JSON_BAD_DATA;
		}

		cur_item = (vod_json_key_value_t*)vod_array_push(result);
		if (cur_item == NULL)
		{
			return VOD_JSON_ALLOC_FAILED;
		}

		rc = vod_json_parse_object_key(state, cur_item);
		if (rc != VOD_JSON_OK)
		{
			return rc;
		}

		vod_json_skip_spaces(state);
		EXPECT_CHAR(state, ':');
		vod_json_skip_spaces(state);

		rc = vod_json_parse_value(state, &cur_item->value);
		if (rc != VOD_JSON_OK)
		{
			return rc;
		}

		vod_json_skip_spaces(state);
		switch (*state->cur_pos)
		{
		case '}':
			state->cur_pos++;
			state->depth--;
			return VOD_JSON_OK;

		case ',':
			state->cur_pos++;
			vod_json_skip_spaces(state);
			continue;
		}

		vod_snprintf(state->error, state->error_size, "expected , or } while parsing object, got 0x%xd%Z", (int)*state->cur_pos);
		return VOD_JSON_BAD_DATA;
	}
}

static vod_json_status_t 
vod_json_parser_string(vod_json_parser_state_t* state, void* result)
{
	ASSERT_CHAR(state, '"');
	return vod_json_parse_string(state, (vod_str_t*)result);
}

static vod_json_status_t
vod_json_parser_array(vod_json_parser_state_t* state, void* result)
{
	ASSERT_CHAR(state, '[');
	return vod_json_parse_array(state, (vod_json_array_t*)result);
}

static vod_json_status_t
vod_json_parser_object(vod_json_parser_state_t* state, void* result)
{
	ASSERT_CHAR(state, '{');
	return vod_json_parse_object(state, (vod_json_object_t*)result);
}

static vod_json_status_t
vod_json_parser_bool(vod_json_parser_state_t* state, void* result)
{
	switch (*state->cur_pos)
	{
	case 't':
		EXPECT_STRING(state, "true");
		*(bool_t*)result = TRUE;
		return VOD_JSON_OK;

	case 'f':
		EXPECT_STRING(state, "false");
		*(bool_t*)result = FALSE;
		return VOD_JSON_OK;
	}

	vod_snprintf(state->error, state->error_size, "expected true or false%Z");
	return VOD_JSON_BAD_DATA;
}

static vod_json_status_t
vod_json_parser_frac(vod_json_parser_state_t* state, void* result)
{
	return vod_json_parse_fraction(state, (vod_json_fraction_t*)result);
}

static vod_json_status_t
vod_json_parser_int(vod_json_parser_state_t* state, void* result)
{
	vod_json_status_t rc;
	bool_t negative;

	rc = vod_json_parse_int(state, (int64_t*)result, &negative);

	if (negative)
	{
		*(int64_t*)result = -(*(int64_t*)result);
	}

	return rc;
}

static vod_json_status_t
vod_json_parse_value(vod_json_parser_state_t* state, vod_json_value_t* result)
{
	vod_json_status_t rc;

	switch (*state->cur_pos)
	{
	case '"':
		result->type = VOD_JSON_STRING;
		return vod_json_parse_string(state, &result->v.str);

	case '[':
		result->type = VOD_JSON_ARRAY;
		return vod_json_parse_array(state, &result->v.arr);

	case '{':
		result->type = VOD_JSON_OBJECT;
		return vod_json_parse_object(state, &result->v.obj);

	case 'n':
		EXPECT_STRING(state, "null");
		result->type = VOD_JSON_NULL;
		return VOD_JSON_OK;

	case 't':
		EXPECT_STRING(state, "true");
		result->type = VOD_JSON_BOOL;
		result->v.boolean = TRUE;
		return VOD_JSON_OK;

	case 'f':
		EXPECT_STRING(state, "false");
		result->type = VOD_JSON_BOOL;
		result->v.boolean = FALSE;
		return VOD_JSON_OK;

	default:
		rc = vod_json_parse_fraction(state, &result->v.num);
		if (rc != VOD_JSON_OK)
		{
			return rc;
		}

		result->type = result->v.num.denom == 1 ? VOD_JSON_INT : VOD_JSON_FRAC;
		return VOD_JSON_OK;
	}
}

vod_json_status_t
vod_json_parse(vod_pool_t* pool, u_char* string, vod_json_value_t* result, u_char* error, size_t error_size)
{
	vod_json_parser_state_t state;
	vod_json_status_t rc;

	state.pool = pool;
	state.cur_pos = string;
	state.depth = 0;
	state.error = error;
	state.error_size = error_size;
	error[0] = '\0';

	vod_json_skip_spaces(&state);
	rc = vod_json_parse_value(&state, result);
	if (rc != VOD_JSON_OK)
	{
		goto error;
	}
	vod_json_skip_spaces(&state);
	if (*state.cur_pos)
	{
		vod_snprintf(error, error_size, "trailing data after json value%Z");
		rc = VOD_JSON_BAD_DATA;
		goto error;
	}

	return VOD_JSON_OK;

error:

	error[error_size - 1] = '\0';			// make sure it's null terminated
	return rc;
}

static u_char*
vod_json_unicode_hex_to_utf8(u_char* dest, u_char* src)
{
	vod_int_t ch;

	ch = vod_hextoi(src, 4);
	if (ch < 0)
	{
		return NULL;
	}

	if (ch < 0x80)
	{
		*dest++ = (u_char)ch;
	}
	else if (ch < 0x800)
	{
		*dest++ = (ch >> 6) | 0xC0;
		*dest++ = (ch & 0x3F) | 0x80;
	}
	else if (ch < 0x10000)
	{
		*dest++ = (ch >> 12) | 0xE0;
		*dest++ = ((ch >> 6) & 0x3F) | 0x80;
		*dest++ = (ch & 0x3F) | 0x80;
	}
	else if (ch < 0x110000)
	{
		*dest++ = (ch >> 18) | 0xF0;
		*dest++ = ((ch >> 12) & 0x3F) | 0x80;
		*dest++ = ((ch >> 6) & 0x3F) | 0x80;
		*dest++ = (ch & 0x3F) | 0x80;
	}
	else
	{
		return NULL;
	}

	return dest;
}

vod_json_status_t
vod_json_decode_string(vod_str_t* dest, vod_str_t* src)
{
	u_char* end_pos;
	u_char* cur_pos;
	u_char* p = dest->data + dest->len;

	cur_pos = src->data;
	end_pos = cur_pos + src->len;
	for (; cur_pos < end_pos; cur_pos++)
	{
		if (*cur_pos != '\\')
		{
			*p++ = *cur_pos;
			continue;
		}

		cur_pos++;
		if (cur_pos >= end_pos)
		{
			return VOD_JSON_BAD_DATA;
		}

		switch (*cur_pos)
		{
		case '"':
			*p++ = '"';
			break;
		case '\\':
			*p++ = '\\';
			break;
		case '/':
			*p++ = '/';
			break;
		case 'b':
			*p++ = '\b';
			break;
		case 'f':
			*p++ = '\f';
			break;
		case 'n':
			*p++ = '\n';
			break;
		case 'r':
			*p++ = '\r';
			break;
		case 't':
			*p++ = '\t';
			break;
		case 'u':
			if (cur_pos + 5 > end_pos)
			{
				return VOD_JSON_BAD_DATA;
			}

			p = vod_json_unicode_hex_to_utf8(p, cur_pos + 1);
			if (p == NULL)
			{
				return VOD_JSON_BAD_DATA;
			}
			cur_pos += 4;
			break;
		default:
			return VOD_JSON_BAD_DATA;
		}
	}

	dest->len = p - dest->data;

	return VOD_OK;
}

vod_status_t
vod_json_init_hash(
	vod_pool_t* pool, 
	vod_pool_t* temp_pool, 
	char* hash_name, 
	void* elements, 
	size_t element_size, 
	vod_hash_t* result)
{
	vod_array_t elements_arr;
	vod_hash_key_t *hash_key;
	vod_hash_init_t hash;
	vod_str_t* cur_key;
	u_char  *element;

	if (vod_array_init(&elements_arr, temp_pool, 32, sizeof(vod_hash_key_t)) != VOD_OK)
	{
		return VOD_ALLOC_FAILED;
	}

	for (element = elements; ; element += element_size)
	{
		cur_key = (vod_str_t*)element;
		if (cur_key->len == 0)
		{
			break;
		}

		hash_key = vod_array_push(&elements_arr);
		if (hash_key == NULL)
		{
			return VOD_ALLOC_FAILED;
		}

		hash_key->key = *cur_key;
		hash_key->key_hash = vod_hash_key_lc(cur_key->data, cur_key->len);
		hash_key->value = element;
	}

	hash.hash = result;
	hash.key = vod_hash_key_lc;
	hash.max_size = 512;
	hash.bucket_size = vod_align(64, vod_cacheline_size);
	hash.name = hash_name;
	hash.pool = pool;
	hash.temp_pool = NULL;

	if (vod_hash_init(&hash, elements_arr.elts, elements_arr.nelts) != VOD_OK) 
	{
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

void
vod_json_get_object_values(
	vod_json_object_t* object, 
	vod_hash_t* values_hash, 
	vod_json_value_t** result)
{
	vod_json_key_value_t* cur_element = object->elts;
	vod_json_key_value_t* last_element = cur_element + object->nelts;
	json_object_key_def_t* key_def;

	for (; cur_element < last_element; cur_element++)
	{
		key_def = vod_hash_find(
			values_hash, 
			cur_element->key_hash, 
			cur_element->key.data, 
			cur_element->key.len);
		if (key_def == NULL)
		{
			continue;
		}

		if (cur_element->value.type == key_def->type ||
			(cur_element->value.type == VOD_JSON_INT && key_def->type == VOD_JSON_FRAC))	// allow passing int for a fraction
		{
			result[key_def->index] = &cur_element->value;
		}
	}
}

vod_status_t
vod_json_parse_object_values(
	vod_json_object_t* object, 
	vod_hash_t* values_hash, 
	void* context, 
	void* result)
{
	vod_json_key_value_t* cur_element = object->elts;
	vod_json_key_value_t* last_element = cur_element + object->nelts;
	json_object_value_def_t* parser;
	vod_status_t rc;

	for (; cur_element < last_element; cur_element++)
	{
		parser = vod_hash_find(
			values_hash,
			cur_element->key_hash,
			cur_element->key.data,
			cur_element->key.len);
		if (parser == NULL)
		{
			continue;
		}

		if (cur_element->value.type != parser->type && 
			(cur_element->value.type != VOD_JSON_INT || parser->type != VOD_JSON_FRAC))
		{
			continue;
		}

		rc = parser->parse(context, &cur_element->value, (u_char*)result + parser->offset);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

vod_status_t
vod_json_parse_union(
	request_context_t* request_context,
	vod_json_object_t* object,
	vod_str_t* type_field,
	vod_uint_t type_field_hash,
	vod_hash_t* union_hash,
	void* context,
	void** dest)
{
	vod_json_key_value_t* cur;
	vod_json_key_value_t* last;
	json_parser_union_type_def_t* type_def;
	vod_str_t type = vod_null_string;
	vod_uint_t key;
	u_char* type_end;
	u_char* cur_pos;
	u_char c;

	// get the object type
	cur = (vod_json_key_value_t*)object->elts;
	last = cur + object->nelts;

	for (; cur < last; cur++)
	{
		if (cur->key_hash != type_field_hash ||
			cur->key.len != type_field->len ||
			vod_memcmp(cur->key.data, type_field->data, type_field->len) != 0)
		{
			continue;
		}

		if (cur->value.type != VOD_JSON_STRING)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"vod_json_parse_union: \"%V\" field has an invalid type %d", type_field, cur->value.type);
			return VOD_BAD_REQUEST;
		}

		type = cur->value.v.str;
		break;
	}

	if (type.len == 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"vod_json_parse_union: missing \"%V\" field", type_field);
		return VOD_BAD_REQUEST;
	}

	// calculate key and to lower
	key = 0;

	type_end = type.data + type.len;
	for (cur_pos = type.data; cur_pos < type_end; cur_pos++)
	{
		c = *cur_pos;
		if (c >= 'A' && c <= 'Z')
		{
			c |= 0x20;			// tolower
			*cur_pos = c;
		}

		key = vod_hash(key, c);
	}

	// find the type definition
	type_def = vod_hash_find(
		union_hash,
		key,
		type.data,
		type.len);
	if (type_def == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"vod_json_parse_union: unknown object type \"%V\"", &type);
		return VOD_BAD_REQUEST;
	}

	return type_def->parser(context, object, dest);
}

static vod_json_key_value_t*
vod_json_get_object_value(vod_json_object_t* object, vod_uint_t key_hash, vod_str_t* key)
{
	vod_json_key_value_t* cur_element = object->elts;
	vod_json_key_value_t* last_element = cur_element + object->nelts;

	for (; cur_element < last_element; cur_element++)
	{
		if (cur_element->key_hash == key_hash &&
			cur_element->key.len == key->len &&
			vod_memcmp(cur_element->key.data, key->data, key->len) == 0)
		{
			return cur_element;
		}
	}

	return NULL;
}

static vod_status_t
vod_json_replace_object(vod_json_object_t* object1, vod_json_object_t* object2)
{
	vod_json_key_value_t* cur_element;
	vod_json_key_value_t* last_element;
	vod_json_key_value_t* dest_element;

	cur_element = object2->elts;
	last_element = cur_element + object2->nelts;
	for (; cur_element < last_element; cur_element++)
	{
		dest_element = vod_json_get_object_value(object1, cur_element->key_hash, &cur_element->key);
		if (dest_element != NULL)
		{
			vod_json_replace(&dest_element->value, &cur_element->value);
			continue;
		}

		dest_element = (vod_json_key_value_t*)vod_array_push(object1);
		if (dest_element == NULL)
		{
			return VOD_ALLOC_FAILED;
		}

		*dest_element = *cur_element;
	}

	return VOD_OK;
}

static vod_status_t
vod_json_replace_array(vod_json_array_t* array1, vod_json_array_t* array2)
{
	vod_json_object_t* cur_object1;
	vod_json_object_t* cur_object2;
	vod_array_part_t* part1;
	vod_array_part_t* part2;
	vod_status_t rc;

	if (array1->type != VOD_JSON_OBJECT || array2->type != VOD_JSON_OBJECT)
	{
		*array1 = *array2;
		return VOD_OK;
	}

	part1 = &array1->part;
	part2 = &array2->part;

	for (cur_object1 = part1->first, cur_object2 = part2->first; 
		; 
		cur_object1++, cur_object2++)
	{
		if ((void*)cur_object2 >= part2->last)
		{
			if (part2->next == NULL)
			{
				break;
			}

			part2 = part2->next;
			cur_object2 = part2->first;
		}

		if ((void*)cur_object1 >= part1->last)
		{
			if (part1->next == NULL)
			{
				// append the second array to the first
				part2->first = cur_object2;
				part2->count = (vod_json_object_t*)part2->last - cur_object2;
				part1->next = part2;
				array1->count = array2->count;
				break;
			}

			part1 = part1->next;
			cur_object1 = part1->first;
		}

		rc = vod_json_replace_object(cur_object1, cur_object2);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

vod_status_t
vod_json_replace(vod_json_value_t* json1, vod_json_value_t* json2)
{
	if (json1->type != json2->type)
	{
		*json1 = *json2;
		return VOD_OK;
	}

	switch (json1->type)
	{
	case VOD_JSON_OBJECT:
		return vod_json_replace_object(&json1->v.obj, &json2->v.obj);

	case VOD_JSON_ARRAY:
		return vod_json_replace_array(&json1->v.arr, &json2->v.arr);

	default:
		*json1 = *json2;
		break;
	}

	return VOD_OK;
}
