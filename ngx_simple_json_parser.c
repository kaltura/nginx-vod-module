#include "ngx_simple_json_parser.h"
#include <ctype.h>

// macros
#define EXPECT_CHAR(state, ch)										\
	if (*(state)->cur_pos != ch)									\
	{																\
		return NGX_JSON_BAD_DATA;									\
	}																\
	(state)->cur_pos++;

#define EXPECT_STRING(state, str)									\
	if (ngx_strncmp((state)->cur_pos, str, sizeof(str) - 1) != 0)	\
	{																\
		return NGX_JSON_BAD_DATA;									\
	}																\
	(state)->cur_pos += sizeof(str) - 1;


// typedefs
typedef struct {
	ngx_pool_t* pool;
	u_char* cur_pos;
} ngx_json_parser_state_t;

// forward declarations
static ngx_int_t ngx_json_parse_value(ngx_json_parser_state_t* state, ngx_json_value_t* result);

static void 
ngx_json_skip_spaces(ngx_json_parser_state_t* state)
{
	for (; *state->cur_pos && isspace(*state->cur_pos); state->cur_pos++);
}

static ngx_int_t 
ngx_json_parse_string(ngx_json_parser_state_t* state, ngx_str_t* result)
{
	EXPECT_CHAR(state, '\"');

	result->data = state->cur_pos;

	for (; *state->cur_pos; state->cur_pos++)
	{
		switch (*state->cur_pos)
		{
		case '\\':
			state->cur_pos++;
			if (!*state->cur_pos)
			{
				return NGX_JSON_BAD_DATA;
			}
			continue;

		case '"':
			result->len = state->cur_pos - result->data;
			state->cur_pos++;
			return NGX_JSON_OK;
		}
	}
	return NGX_JSON_BAD_DATA;
}

static ngx_int_t
ngx_json_parse_array(ngx_json_parser_state_t* state, ngx_array_t* result)
{
	ngx_json_value_t* cur_item;
	ngx_int_t rc;

	rc = ngx_array_init(result, state->pool, 5, sizeof(*cur_item));
	if (rc != NGX_OK)
	{
		return NGX_JSON_ALLOC_FAILED;
	}

	EXPECT_CHAR(state, '[');
	ngx_json_skip_spaces(state);
	if (*state->cur_pos == ']')
	{
		state->cur_pos++;
		return NGX_JSON_OK;
	}

	for (;;)
	{
		cur_item = (ngx_json_value_t*)ngx_array_push(result);
		if (cur_item == NULL)
		{
			return NGX_JSON_ALLOC_FAILED;
		}

		rc = ngx_json_parse_value(state, cur_item);
		if (rc != NGX_JSON_OK)
		{
			return rc;
		}

		ngx_json_skip_spaces(state);
		switch (*state->cur_pos)
		{
		case ']':
			state->cur_pos++;
			return NGX_JSON_OK;

		case ',':
			state->cur_pos++;
			ngx_json_skip_spaces(state);
			continue;
		}

		return NGX_JSON_BAD_DATA;
	}
}

static ngx_int_t
ngx_json_parse_object(ngx_json_parser_state_t* state, ngx_array_t* result)
{
	ngx_json_key_value_t* cur_item;
	ngx_int_t rc;

	rc = ngx_array_init(result, state->pool, 5, sizeof(*cur_item));
	if (rc != NGX_OK)
	{
		return NGX_JSON_ALLOC_FAILED;
	}

	EXPECT_CHAR(state, '{');
	ngx_json_skip_spaces(state);
	if (*state->cur_pos == '}')
	{
		state->cur_pos++;
		return NGX_JSON_OK;
	}

	for (;;)
	{ 
		cur_item = (ngx_json_key_value_t*)ngx_array_push(result);
		if (cur_item == NULL)
		{
			return NGX_JSON_ALLOC_FAILED;
		}

		rc = ngx_json_parse_string(state, &cur_item->key);
		if (rc != NGX_JSON_OK)
		{
			return rc;
		}

		ngx_json_skip_spaces(state);
		EXPECT_CHAR(state, ':');
		ngx_json_skip_spaces(state);

		rc = ngx_json_parse_value(state, &cur_item->value);
		if (rc != NGX_JSON_OK)
		{
			return rc;
		}

		ngx_json_skip_spaces(state);
		switch (*state->cur_pos)
		{
		case '}':
			state->cur_pos++;
			return NGX_JSON_OK;

		case ',':
			state->cur_pos++;
			ngx_json_skip_spaces(state);
			continue;
		}

		return NGX_JSON_BAD_DATA;
	}
}

static ngx_int_t
ngx_json_parse_value(ngx_json_parser_state_t* state, ngx_json_value_t* result)
{
	switch (*state->cur_pos)
	{
	case '"':
		result->type = NGX_JSON_STRING;
		return ngx_json_parse_string(state, &result->v.str);

	case '[':
		result->type = NGX_JSON_ARRAY;
		return ngx_json_parse_array(state, &result->v.arr);

	case '{':
		result->type = NGX_JSON_OBJECT;
		return ngx_json_parse_object(state, &result->v.obj);

	case 'n':
		result->type = NGX_JSON_NULL;
		EXPECT_STRING(state, "null");
		return NGX_JSON_OK;

	case 't':
		result->type = NGX_JSON_TRUE;
		EXPECT_STRING(state, "true");
		return NGX_JSON_OK;

	case 'f':
		result->type = NGX_JSON_FALSE;
		EXPECT_STRING(state, "false");
		return NGX_JSON_OK;

	// TODO: add support for numbers
	}

	return NGX_JSON_BAD_DATA;
}

ngx_int_t
ngx_json_parse(ngx_pool_t* pool, u_char* string, ngx_json_value_t* result)
{
	ngx_json_parser_state_t state;
	ngx_int_t rc;

	state.pool = pool;
	state.cur_pos = string;

	ngx_json_skip_spaces(&state);
	rc = ngx_json_parse_value(&state, result);
	if (rc != NGX_JSON_OK)
	{
		return rc;
	}
	ngx_json_skip_spaces(&state);
	if (*state.cur_pos)
	{
		return NGX_JSON_BAD_DATA;
	}

	return NGX_JSON_OK;
}

static ngx_int_t
ngx_json_get_element(ngx_json_value_t* object, ngx_str_t* key, int expected_type, ngx_json_value_t** result)
{
	ngx_json_key_value_t* cur;
	ngx_json_key_value_t* last;

	if (object->type != NGX_JSON_OBJECT)
	{
		return NGX_JSON_BAD_TYPE;
	}

	cur = (ngx_json_key_value_t*)object->v.obj.elts;
	last = cur + object->v.obj.nelts;
	for (; cur < last; cur++)
	{
		if (cur->key.len == key->len &&
			ngx_memcmp(cur->key.data, key->data, key->len) == 0)
		{
			if (cur->value.type != expected_type)
			{
				return NGX_JSON_BAD_TYPE;
			}

			*result = &cur->value;
			return NGX_JSON_OK;
		}
	}

	return NGX_JSON_NOT_FOUND;
}

static int
ngx_json_get_hex_char_value(int ch)
{
	if (ch >= '0' && ch <= '9') {
		return (ch - '0');
	}

	ch = (ch | 0x20);		// lower case

	if (ch >= 'a' && ch <= 'f') {
		return (ch - 'a' + 10);
	}

	return -1;
}

ngx_int_t
ngx_json_get_element_guid_string(ngx_json_value_t* object, ngx_str_t* key, u_char* output)
{
	ngx_json_value_t* element;
	ngx_int_t rc;
	u_char* cur_pos;
	u_char* end_pos;
	u_char* output_end = output + NGX_GUID_LEN;
	int c1;
	int c2;

	rc = ngx_json_get_element(object, key, NGX_JSON_STRING, &element);
	if (rc != NGX_JSON_OK)
	{
		return rc;
	}
	
	cur_pos = element->v.str.data;
	end_pos = cur_pos + element->v.str.len;
	while (cur_pos + 1 < end_pos)
	{
		if (*cur_pos == '-')
		{
			cur_pos++;
			continue;
		}

		c1 = ngx_json_get_hex_char_value(cur_pos[0]);
		c2 = ngx_json_get_hex_char_value(cur_pos[1]);
		if (c1 < 0 || c2 < 0)
		{
			return NGX_JSON_BAD_DATA;
		}

		if (output >= output_end)
		{
			return NGX_JSON_BAD_LENGTH;
		}

		*output++ = ((c1 << 4) | c2);
		cur_pos += 2;
	}

	if (output < output_end)
	{
		return NGX_JSON_BAD_LENGTH;
	}

	return NGX_JSON_OK;
}

static ngx_int_t
ngx_base64_exact_decoded_length(ngx_str_t* base64, size_t* decoded_len)
{
	size_t padding_size;
	u_char *cur_pos;

	if ((base64->len & 3) != 0)
	{
		return NGX_JSON_BAD_LENGTH;
	}

	padding_size = 0;
	for (cur_pos = base64->data + base64->len - 1; cur_pos >= base64->data; cur_pos--)
	{
		if (*cur_pos != '=')
		{
			break;
		}
		padding_size++;
	}

	if (padding_size > 2)
	{
		return NGX_JSON_BAD_DATA;
	}

	*decoded_len = (base64->len >> 2) * 3 - padding_size;
	return NGX_JSON_OK;
}

ngx_int_t
ngx_json_get_element_fixed_binary_string(ngx_json_value_t* object, ngx_str_t* key, u_char* output, size_t output_size)
{
	ngx_json_value_t* element;
	ngx_str_t output_str;
	ngx_int_t rc;
	size_t decoded_len;

	rc = ngx_json_get_element(object, key, NGX_JSON_STRING, &element);
	if (rc != NGX_JSON_OK)
	{
		return rc;
	}

	rc = ngx_base64_exact_decoded_length(&element->v.str, &decoded_len);
	if (rc != NGX_JSON_OK)
	{
		return rc;
	}

	if (decoded_len != output_size)
	{
		return NGX_JSON_BAD_LENGTH;
	}

	output_str.data = output;
	if (ngx_decode_base64(&output_str, &element->v.str) != NGX_OK)
	{
		return NGX_JSON_BAD_DATA;
	}

	if (output_str.len != output_size)
	{
		return NGX_JSON_BAD_LENGTH;
	}

	return NGX_JSON_OK;
}

ngx_int_t
ngx_json_get_element_binary_string(ngx_pool_t* pool, ngx_json_value_t* object, ngx_str_t* key, ngx_str_t* result)
{
	ngx_json_value_t* element;
	ngx_int_t rc;

	rc = ngx_json_get_element(object, key, NGX_JSON_STRING, &element);
	if (rc != NGX_JSON_OK)
	{
		return rc;
	}

	result->data = ngx_palloc(pool, ngx_base64_decoded_length(element->v.str.len));
	if (result->data == NULL)
	{
		return NGX_JSON_ALLOC_FAILED;
	}

	if (ngx_decode_base64(result, &element->v.str) != NGX_OK)
	{
		return NGX_JSON_BAD_DATA;
	}

	return NGX_JSON_OK;
}

ngx_int_t 
ngx_json_get_element_array(ngx_json_value_t* object, ngx_str_t* key, ngx_array_t** result)
{
	ngx_json_value_t* element;
	ngx_int_t rc;

	rc = ngx_json_get_element(object, key, NGX_JSON_ARRAY, &element);
	if (rc != NGX_JSON_OK)
	{
		return rc;
	}

	*result = &element->v.arr;

	return NGX_JSON_OK;
}
