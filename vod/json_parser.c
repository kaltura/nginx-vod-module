#include "json_parser.h"
#include <ctype.h>

// constants
#define MAX_JSON_ELEMENTS (1024)
#define MAX_RECURSION_DEPTH (32)

// macros
#define EXPECT_CHAR(state, ch)										\
	if (*(state)->cur_pos != ch)									\
	{																\
		vod_snprintf(state->error, state->error_size, "expected 0x%xd got 0x%xd%Z", (int)ch, (int)*(state)->cur_pos); \
		return VOD_JSON_BAD_DATA;									\
	}																\
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
	u_char* error;
	size_t error_size;
} vod_json_parser_state_t;

// forward declarations
static vod_json_status_t vod_json_parse_value(vod_json_parser_state_t* state, vod_json_value_t* result, int depth);

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
vod_json_parse_number(vod_json_parser_state_t* state, vod_json_value_t* result)
{
	int64_t value;
	uint64_t denom = 1;
	bool_t negative;

	if (*state->cur_pos == '-')
	{
		negative = 1;
		state->cur_pos++;
	}
	else
	{
		negative = 0;
	}

	if (!isdigit(*state->cur_pos))
	{
		vod_snprintf(state->error, state->error_size, "expected digit got 0x%xd%Z", (int)*state->cur_pos);
		return VOD_BAD_DATA;
	}

	value = 0;

	do
	{
		if (value > LLONG_MAX / 10 - 1)
		{
			vod_snprintf(state->error, state->error_size, "number value overflow (1)%Z");
			return VOD_BAD_DATA;
		}

		value = value * 10 + (*state->cur_pos - '0');
		state->cur_pos++;
	} while (isdigit(*state->cur_pos));

	if (*state->cur_pos == '.')
	{
		state->cur_pos++;

		if (!isdigit(*state->cur_pos))
		{
			vod_snprintf(state->error, state->error_size, "expected digit got 0x%xd%Z", (int)*state->cur_pos);
			return VOD_BAD_DATA;
		}

		result->type = VOD_JSON_FRAC;

		do
		{
			if (value > LLONG_MAX / 10 - 1 || denom > ULLONG_MAX / 10)
			{
				vod_snprintf(state->error, state->error_size, "number value overflow (2)%Z");
				return VOD_BAD_DATA;
			}

			value = value * 10 + (*state->cur_pos - '0');
			denom *= 10;
			state->cur_pos++;
		} while (isdigit(*state->cur_pos));
	}
	else
	{
		result->type = VOD_JSON_INT;
	}

	if (negative)
	{
		value = -value;
	}

	result->v.num.nom = value;
	result->v.num.denom = denom;

	return VOD_OK;
}

static vod_json_status_t
vod_json_parse_array(vod_json_parser_state_t* state, vod_array_t* result, int depth)
{
	vod_json_value_t* cur_item;
	vod_status_t rc;

	state->cur_pos++;		// skip the [
	vod_json_skip_spaces(state);
	if (*state->cur_pos == ']')
	{
		result->nelts = 0;
		result->size = sizeof(*cur_item);
		result->nalloc = 0;
		result->pool = state->pool;
		result->elts = NULL;

		state->cur_pos++;
		return VOD_JSON_OK;
	}

	rc = vod_array_init(result, state->pool, 5, sizeof(*cur_item));
	if (rc != VOD_OK)
	{
		return VOD_JSON_ALLOC_FAILED;
	}

	for (;;)
	{
		if (result->nelts >= MAX_JSON_ELEMENTS)
		{
			vod_snprintf(state->error, state->error_size, "array elements count exceeds the limit%Z");
			return VOD_JSON_BAD_DATA;
		}

		cur_item = (vod_json_value_t*)vod_array_push(result);
		if (cur_item == NULL)
		{
			return VOD_JSON_ALLOC_FAILED;
		}

		rc = vod_json_parse_value(state, cur_item, depth);
		if (rc != VOD_JSON_OK)
		{
			return rc;
		}

		vod_json_skip_spaces(state);
		switch (*state->cur_pos)
		{
		case ']':
			state->cur_pos++;
			return VOD_JSON_OK;

		case ',':
			state->cur_pos++;
			vod_json_skip_spaces(state);
			continue;
		}

		vod_snprintf(state->error, state->error_size, "expected , or ] while parsing array, got 0x%xd%Z", (int)*state->cur_pos);
		return VOD_JSON_BAD_DATA;
	}
}

static vod_json_status_t
vod_json_parse_object(vod_json_parser_state_t* state, vod_array_t* result, int depth)
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

		rc = vod_json_parse_value(state, &cur_item->value, depth);
		if (rc != VOD_JSON_OK)
		{
			return rc;
		}

		vod_json_skip_spaces(state);
		switch (*state->cur_pos)
		{
		case '}':
			state->cur_pos++;
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
vod_json_parse_value(vod_json_parser_state_t* state, vod_json_value_t* result, int depth)
{
	if (depth >= MAX_RECURSION_DEPTH)
	{
		vod_snprintf(state->error, state->error_size, "max recursion depth exceeded%Z");
		return VOD_JSON_BAD_DATA;
	}

	switch (*state->cur_pos)
	{
	case '"':
		result->type = VOD_JSON_STRING;
		return vod_json_parse_string(state, &result->v.str);

	case '[':
		result->type = VOD_JSON_ARRAY;
		return vod_json_parse_array(state, &result->v.arr, depth + 1);

	case '{':
		result->type = VOD_JSON_OBJECT;
		return vod_json_parse_object(state, &result->v.obj, depth + 1);

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
		return vod_json_parse_number(state, result);
	}
}

vod_json_status_t
vod_json_parse(vod_pool_t* pool, u_char* string, vod_json_value_t* result, u_char* error, size_t error_size)
{
	vod_json_parser_state_t state;
	vod_json_status_t rc;

	state.pool = pool;
	state.cur_pos = string;
	state.error = error;
	state.error_size = error_size;
	error[0] = '\0';

	vod_json_skip_spaces(&state);
	rc = vod_json_parse_value(&state, result, 0);
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
	vod_json_value_t* object, 
	vod_hash_t* values_hash, 
	vod_json_value_t** result)
{
	vod_json_key_value_t* cur_element = object->v.obj.elts;
	vod_json_key_value_t* last_element = cur_element + object->v.obj.nelts;
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
	vod_json_value_t* object, 
	vod_hash_t* values_hash, 
	void* context, 
	void* result)
{
	vod_json_key_value_t* cur_element = object->v.obj.elts;
	vod_json_key_value_t* last_element = cur_element + object->v.obj.nelts;
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
	vod_json_value_t* object,
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
	cur = (vod_json_key_value_t*)object->v.obj.elts;
	last = cur + object->v.obj.nelts;

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
