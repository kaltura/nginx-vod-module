#include <inttypes.h>
#include <stdio.h>
#include <ngx_core.h>
#include <vod/json_parser.h>
#include <vod/parse_utils.h>

volatile ngx_cycle_t  *ngx_cycle;
ngx_pool_t *pool;
ngx_log_t ngx_log;

#if (NGX_HAVE_VARIADIC_MACROS)

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)

#else

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, va_list args)

#endif
{
}

void*
ngx_array_push(ngx_array_t *a)
{
    void        *elt, *new_elts;

	if (a->nelts >= a->nalloc)
	{
		new_elts = realloc(a->elts, a->size * a->nalloc * 2);
		if (new_elts == NULL)
		{
			return NULL;
		}
		a->elts = new_elts;
		a->nalloc *= 2;
	}
	
    elt = (u_char *) a->elts + a->size * a->nelts;
    a->nelts++;

    return elt;
}

#define assert(cond) if (!(cond)) { printf("Error: assertion failed, file=%s line=%d\n", __FILE__, __LINE__); }
#define assert_string(val, expected) assert(val.len == sizeof(expected) - 1 && memcmp(val.data, expected, sizeof(expected) - 1) == 0)

void sanity_tests()
{
	vod_json_key_value_t* pairs;
	vod_json_value_t* elements;
	vod_json_value_t result;
	ngx_int_t rc;
	u_char error[128];

	rc = vod_json_parse(pool, (u_char*)" null ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_NULL);

	rc = vod_json_parse(pool, (u_char*)" true ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_BOOL && result.v.boolean);

	rc = vod_json_parse(pool, (u_char*)" false ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_BOOL && !result.v.boolean);

	rc = vod_json_parse(pool, (u_char*)" \"test\" ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_STRING);
	assert_string(result.v.str, "test");
	
	rc = vod_json_parse(pool, (u_char*)" \"fsdaf\\\"fsaf\nfdasf\\fdfas\" ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_STRING);
	assert_string(result.v.str, "fsdaf\\\"fsaf\nfdasf\\fdfas");

	rc = vod_json_parse(pool, (u_char*)" [ ] ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_ARRAY);
	assert(result.v.arr.nelts == 0);

	rc = vod_json_parse(pool, (u_char*)" [ null , true , false , \"test\" ] ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_ARRAY);
	assert(result.v.arr.nelts == 4);
	elements = (vod_json_value_t*)result.v.arr.elts;
	assert(elements[0].type == VOD_JSON_NULL);
	assert(elements[1].type == VOD_JSON_BOOL && elements[1].v.boolean);
	assert(elements[2].type == VOD_JSON_BOOL && !elements[2].v.boolean);
	assert(elements[3].type == VOD_JSON_STRING);
	assert_string(elements[3].v.str, "test");

	rc = vod_json_parse(pool, (u_char*)" [ [ null ] ] ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_ARRAY);
	assert(result.v.arr.nelts == 1);
	elements = (vod_json_value_t*)result.v.arr.elts;
	assert(elements[0].type == VOD_JSON_ARRAY);
	elements = (vod_json_value_t*)elements[0].v.arr.elts;
	assert(elements[0].type == VOD_JSON_NULL);

	rc = vod_json_parse(pool, (u_char*)" [ [ null ] , false ] ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_ARRAY);
	assert(result.v.arr.nelts == 2);
	elements = (vod_json_value_t*)result.v.arr.elts;
	assert(elements[1].type == VOD_JSON_BOOL && !elements[1].v.boolean);
	assert(elements[0].type == VOD_JSON_ARRAY);
	elements = (vod_json_value_t*)elements[0].v.arr.elts;
	assert(elements[0].type == VOD_JSON_NULL);

	rc = vod_json_parse(pool, (u_char*)" { } ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_OBJECT);
	assert(result.v.obj.nelts == 0);

	rc = vod_json_parse(pool, (u_char*)" { \"key1\" : null , \"key2\" : true , \"key3\" : false , \"key4\" : \"value\" }", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_OBJECT);
	assert(result.v.arr.nelts == 4);
	pairs = (vod_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[0].key, "key1");
	assert_string(pairs[1].key, "key2");
	assert_string(pairs[2].key, "key3");
	assert_string(pairs[3].key, "key4");
	assert(pairs[0].value.type == VOD_JSON_NULL);
	assert(pairs[1].value.type == VOD_JSON_BOOL && pairs[1].value.v.boolean);
	assert(pairs[2].value.type == VOD_JSON_BOOL && !pairs[2].value.v.boolean);
	assert(pairs[3].value.type == VOD_JSON_STRING);
	assert_string(pairs[3].value.v.str, "value");
	
	rc = vod_json_parse(pool, (u_char*)" { \"key\" : { \"subkey\": \"value\" } } ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_OBJECT);
	assert(result.v.obj.nelts == 1);
	pairs = (vod_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[0].key, "key");
	assert(pairs[0].value.type == VOD_JSON_OBJECT);
	assert(pairs[0].value.v.obj.nelts == 1);
	pairs = (vod_json_key_value_t*)pairs[0].value.v.arr.elts;
	assert_string(pairs[0].key, "subkey");
	assert(pairs[0].value.type == VOD_JSON_STRING);
	assert_string(pairs[0].value.v.str, "value");
	
	rc = vod_json_parse(pool, (u_char*)" { \"key1\" : { \"subkey\": \"value\" } , \"key2\" : null } ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_OBJECT);
	assert(result.v.obj.nelts == 2);
	pairs = (vod_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[1].key, "key2");
	assert(pairs[1].value.type == VOD_JSON_NULL);
	assert_string(pairs[0].key, "key1");
	assert(pairs[0].value.type == VOD_JSON_OBJECT);
	assert(pairs[0].value.v.obj.nelts == 1);
	pairs = (vod_json_key_value_t*)pairs[0].value.v.arr.elts;
	assert_string(pairs[0].key, "subkey");
	assert(pairs[0].value.type == VOD_JSON_STRING);
	assert_string(pairs[0].value.v.str, "value");

	rc = vod_json_parse(pool, (u_char*)" { \"key\" : [ null ] } ", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_OBJECT);
	assert(result.v.obj.nelts == 1);
	pairs = (vod_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[0].key, "key");
	assert(pairs[0].value.type == VOD_JSON_ARRAY);
	elements = (vod_json_value_t*)pairs[0].value.v.arr.elts;
	assert(elements[0].type == VOD_JSON_NULL);

	rc = vod_json_parse(pool, (u_char*)" [ { \"key\" : null } ]", &result, error, sizeof(error));
	assert(rc == VOD_JSON_OK);
	assert(result.type == VOD_JSON_ARRAY);
	assert(result.v.arr.nelts == 1);
	elements = (vod_json_value_t*)result.v.arr.elts;
	assert(elements[0].type == VOD_JSON_OBJECT);	
	pairs = (vod_json_key_value_t*)elements[0].v.arr.elts;
	assert_string(pairs[0].key, "key");
	assert(pairs[0].value.type == VOD_JSON_NULL);
}

void bad_jsons_test()
{
	static char* tests[] = {
		"",
		" ",
		"x",
		"tru",
		"\"fasdf",
		"\"fdasf\\",
		"[\"fdaf]",
		"[\"fdafa\"",
		"[\"fdafa\"}",
		"[\"fdafas\",]",
		"{\"fasd:null}",
		"{\"fasd\",null}",
		"{\"fasd\",\"fdasf}",
		"{\"fasd\",\"fdasf\"]",
		"{\"fasd\",\"fdasf\",}",
		"\"fdsafas\"  x",
		NULL
	};
	vod_json_value_t result;
	char** cur_test;
	ngx_int_t expected_rc = VOD_JSON_BAD_DATA;
	ngx_int_t rc;
	u_char error[128];

	for (cur_test = tests; *cur_test; cur_test++)
	{
		rc = vod_json_parse(pool, (u_char*)*cur_test, &result, error, sizeof(error));
		if (rc != expected_rc)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", *cur_test, expected_rc, rc);
		}
	}
}

void get_element_guid_tests()
{
	static ngx_str_t tests[] = {
		ngx_string("xx"),
		ngx_string("0000000000000000000000000000000000"),
		ngx_string("0000000000000000000000000000"),
		ngx_null_string
	};
	u_char guid[16];
	ngx_str_t* cur_test;
	ngx_int_t rc;

	for (cur_test = tests; cur_test->len; cur_test++)
	{
		rc = parse_utils_parse_guid_string(cur_test, guid);
		if (rc != VOD_BAD_DATA)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->data, (vod_status_t)VOD_BAD_DATA, rc);
		}
	}
}

void get_fixed_string_tests()
{
	static ngx_str_t tests[] = {
		ngx_string("123"), 
		ngx_string("12345==="), 
		ngx_string("123456=="),
		ngx_string("2xF1CWaBQs21ihR4NI-AwQ=="),
		ngx_string("2xF1CWaBQs21ihR4NI=AwQ=="),
		ngx_null_string,
	};
	ngx_str_t* cur_test;
	u_char str[16];
	ngx_int_t rc;
	
	for (cur_test = tests; cur_test->len; cur_test++)
	{
		rc = parse_utils_parse_fixed_base64_string(cur_test, str, sizeof(str));
		if (rc != VOD_BAD_DATA)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->data, (vod_status_t)VOD_BAD_DATA, rc);
		}
	}
}

void get_binary_string_tests()
{
	static ngx_str_t tests[] = {
		ngx_string("2xF1CWaBQs21ihR4NI-AwQ=="),
		ngx_null_string,
	};
	ngx_str_t* cur_test;
	ngx_str_t str;
	ngx_int_t rc;
	
	for (cur_test = tests; cur_test->len; cur_test++)
	{		
		rc = parse_utils_parse_variable_base64_string(pool, cur_test, &str);
		if (rc != VOD_BAD_DATA)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->data, (vod_status_t)VOD_BAD_DATA, rc);
		}
	}
}

int main()
{
	pool = ngx_create_pool(1024 * 1024, &ngx_log);
	
	sanity_tests();
	bad_jsons_test();
	get_element_guid_tests();
	get_fixed_string_tests();
	get_binary_string_tests();
	return 0;
}
