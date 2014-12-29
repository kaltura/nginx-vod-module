#include <inttypes.h>
#include <stdio.h>
#include <ngx_core.h>
#include "ngx_simple_json_parser.h"

void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
	return malloc(size);
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

static ngx_int_t
ngx_decode_base64_internal(ngx_str_t *dst, ngx_str_t *src, const u_char *basis)
{
    size_t          len;
    u_char         *d, *s;

    for (len = 0; len < src->len; len++) {
        if (src->data[len] == '=') {
            break;
        }

        if (basis[src->data[len]] == 77) {
            return NGX_ERROR;
        }
    }

    if (len % 4 == 1) {
        return NGX_ERROR;
    }

    s = src->data;
    d = dst->data;

    while (len > 3) {
        *d++ = (u_char) (basis[s[0]] << 2 | basis[s[1]] >> 4);
        *d++ = (u_char) (basis[s[1]] << 4 | basis[s[2]] >> 2);
        *d++ = (u_char) (basis[s[2]] << 6 | basis[s[3]]);

        s += 4;
        len -= 4;
    }

    if (len > 1) {
        *d++ = (u_char) (basis[s[0]] << 2 | basis[s[1]] >> 4);
    }

    if (len > 2) {
        *d++ = (u_char) (basis[s[1]] << 4 | basis[s[2]] >> 2);
    }

    dst->len = d - dst->data;

    return NGX_OK;
}

ngx_int_t
ngx_decode_base64(ngx_str_t *dst, ngx_str_t *src)
{
    static u_char   basis64[] = {
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 62, 77, 77, 77, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 77, 77, 77, 77, 77, 77,
        77,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 77, 77, 77, 77, 77,
        77, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 77, 77, 77, 77, 77,

        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
        77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77
    };

    return ngx_decode_base64_internal(dst, src, basis64);
}


#define assert(cond) if (!(cond)) { printf("Error: assertion failed, file=%s line=%d\n", __FILE__, __LINE__); }
#define assert_string(val, expected) assert(val.len == sizeof(expected) - 1 && memcmp(val.data, expected, sizeof(expected) - 1) == 0)

void sanity_tests()
{
	ngx_json_key_value_t* pairs;
	ngx_json_value_t* elements;
	ngx_json_value_t result;
	ngx_int_t rc;

	rc = ngx_json_parse(NULL, (u_char*)" null ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_NULL);

	rc = ngx_json_parse(NULL, (u_char*)" true ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_TRUE);

	rc = ngx_json_parse(NULL, (u_char*)" false ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_FALSE);

	rc = ngx_json_parse(NULL, (u_char*)" \"test\" ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_STRING);
	assert_string(result.v.str, "test");
	
	rc = ngx_json_parse(NULL, (u_char*)" \"fsdaf\\\"fsaf\nfdasf\\fdfas\" ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_STRING);
	assert_string(result.v.str, "fsdaf\\\"fsaf\nfdasf\\fdfas");

	rc = ngx_json_parse(NULL, (u_char*)" [ ] ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_ARRAY);
	assert(result.v.arr.nelts == 0);

	rc = ngx_json_parse(NULL, (u_char*)" [ null , true , false , \"test\" ] ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_ARRAY);
	assert(result.v.arr.nelts == 4);
	elements = (ngx_json_value_t*)result.v.arr.elts;
	assert(elements[0].type == NGX_JSON_NULL);
	assert(elements[1].type == NGX_JSON_TRUE);
	assert(elements[2].type == NGX_JSON_FALSE);
	assert(elements[3].type == NGX_JSON_STRING);
	assert_string(elements[3].v.str, "test");

	rc = ngx_json_parse(NULL, (u_char*)" [ [ null ] ] ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_ARRAY);
	assert(result.v.arr.nelts == 1);
	elements = (ngx_json_value_t*)result.v.arr.elts;
	assert(elements[0].type == NGX_JSON_ARRAY);
	elements = (ngx_json_value_t*)elements[0].v.arr.elts;
	assert(elements[0].type == NGX_JSON_NULL);

	rc = ngx_json_parse(NULL, (u_char*)" [ [ null ] , false ] ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_ARRAY);
	assert(result.v.arr.nelts == 2);
	elements = (ngx_json_value_t*)result.v.arr.elts;
	assert(elements[1].type == NGX_JSON_FALSE);
	assert(elements[0].type == NGX_JSON_ARRAY);
	elements = (ngx_json_value_t*)elements[0].v.arr.elts;
	assert(elements[0].type == NGX_JSON_NULL);

	rc = ngx_json_parse(NULL, (u_char*)" { } ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_OBJECT);
	assert(result.v.obj.nelts == 0);

	rc = ngx_json_parse(NULL, (u_char*)" { \"key1\" : null , \"key2\" : true , \"key3\" : false , \"key4\" : \"value\" }", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_OBJECT);
	assert(result.v.arr.nelts == 4);
	pairs = (ngx_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[0].key, "key1");
	assert_string(pairs[1].key, "key2");
	assert_string(pairs[2].key, "key3");
	assert_string(pairs[3].key, "key4");
	assert(pairs[0].value.type == NGX_JSON_NULL);
	assert(pairs[1].value.type == NGX_JSON_TRUE);
	assert(pairs[2].value.type == NGX_JSON_FALSE);
	assert(pairs[3].value.type == NGX_JSON_STRING);
	assert_string(pairs[3].value.v.str, "value");
	
	rc = ngx_json_parse(NULL, (u_char*)" { \"key\" : { \"subkey\": \"value\" } } ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_OBJECT);
	assert(result.v.obj.nelts == 1);
	pairs = (ngx_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[0].key, "key");
	assert(pairs[0].value.type == NGX_JSON_OBJECT);
	assert(pairs[0].value.v.obj.nelts == 1);
	pairs = (ngx_json_key_value_t*)pairs[0].value.v.arr.elts;
	assert_string(pairs[0].key, "subkey");
	assert(pairs[0].value.type == NGX_JSON_STRING);
	assert_string(pairs[0].value.v.str, "value");
	
	rc = ngx_json_parse(NULL, (u_char*)" { \"key1\" : { \"subkey\": \"value\" } , \"key2\" : null } ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_OBJECT);
	assert(result.v.obj.nelts == 2);
	pairs = (ngx_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[1].key, "key2");
	assert(pairs[1].value.type == NGX_JSON_NULL);
	assert_string(pairs[0].key, "key1");
	assert(pairs[0].value.type == NGX_JSON_OBJECT);
	assert(pairs[0].value.v.obj.nelts == 1);
	pairs = (ngx_json_key_value_t*)pairs[0].value.v.arr.elts;
	assert_string(pairs[0].key, "subkey");
	assert(pairs[0].value.type == NGX_JSON_STRING);
	assert_string(pairs[0].value.v.str, "value");

	rc = ngx_json_parse(NULL, (u_char*)" { \"key\" : [ null ] } ", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_OBJECT);
	assert(result.v.obj.nelts == 1);
	pairs = (ngx_json_key_value_t*)result.v.arr.elts;
	assert_string(pairs[0].key, "key");
	assert(pairs[0].value.type == NGX_JSON_ARRAY);
	elements = (ngx_json_value_t*)pairs[0].value.v.arr.elts;
	assert(elements[0].type == NGX_JSON_NULL);

	rc = ngx_json_parse(NULL, (u_char*)" [ { \"key\" : null } ]", &result);
	assert(rc == NGX_JSON_OK);
	assert(result.type == NGX_JSON_ARRAY);
	assert(result.v.arr.nelts == 1);
	elements = (ngx_json_value_t*)result.v.arr.elts;
	assert(elements[0].type == NGX_JSON_OBJECT);	
	pairs = (ngx_json_key_value_t*)elements[0].v.arr.elts;
	assert_string(pairs[0].key, "key");
	assert(pairs[0].value.type == NGX_JSON_NULL);
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
	ngx_json_value_t result;
	char** cur_test;
	ngx_int_t expected_rc = NGX_JSON_BAD_DATA;
	ngx_int_t rc;

	for (cur_test = tests; *cur_test; cur_test++)
	{
		rc = ngx_json_parse(NULL, (u_char*)*cur_test, &result);
		if (rc != expected_rc)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", *cur_test, expected_rc, rc);
		}
	}
}

typedef struct {
	char* test;
	ngx_int_t expected_rc;
} test_result_t;

ngx_str_t element = ngx_string("element");

void get_element_guid_tests()
{
	static test_result_t tests[] = {
		{"{\"elemen\":null}", NGX_JSON_NOT_FOUND},
		{"{\"element\":[]}", NGX_JSON_BAD_TYPE},
		{"{\"element\":\"xx\"}", NGX_JSON_BAD_DATA},
		{"{\"element\":\"0000000000000000000000000000000000\"}", NGX_JSON_BAD_LENGTH},
		{"{\"element\":\"0000000000000000000000000000\"}", NGX_JSON_BAD_LENGTH},
		{NULL, 0},
	};
	ngx_json_value_t result;
	test_result_t* cur_test;
	u_char guid[NGX_GUID_LEN];
	ngx_int_t rc;
	
	for (cur_test = tests; cur_test->test; cur_test++)
	{
		rc = ngx_json_parse(NULL, (u_char*)cur_test->test, &result);
		assert(rc == NGX_JSON_OK);
		
		rc = ngx_json_get_element_guid_string(&result, &element, guid);
		if (rc != cur_test->expected_rc)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->test, cur_test->expected_rc, rc);
		}
	}
}

void get_fixed_string_tests()
{
	static test_result_t tests[] = {
		{"{\"elemen\":null}", NGX_JSON_NOT_FOUND},
		{"{\"element\":[]}", NGX_JSON_BAD_TYPE},
		{"{\"element\":\"123\"}", NGX_JSON_BAD_LENGTH},
		{"{\"element\":\"12345===\"}", NGX_JSON_BAD_DATA},
		{"{\"element\":\"123456==\"}", NGX_JSON_BAD_LENGTH},
		{"{\"element\":\"2xF1CWaBQs21ihR4NI-AwQ==\"}", NGX_JSON_BAD_DATA},
		{"{\"element\":\"2xF1CWaBQs21ihR4NI=AwQ==\"}", NGX_JSON_BAD_LENGTH},
		{NULL, 0},
	};
	ngx_json_value_t result;
	test_result_t* cur_test;
	u_char str[16];
	ngx_int_t rc;
	
	for (cur_test = tests; cur_test->test; cur_test++)
	{
		rc = ngx_json_parse(NULL, (u_char*)cur_test->test, &result);
		assert(rc == NGX_JSON_OK);
		
		rc = ngx_json_get_element_fixed_binary_string(&result, &element, str, sizeof(str));
		if (rc != cur_test->expected_rc)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->test, cur_test->expected_rc, rc);
		}
	}
}

void get_binary_string_tests()
{
	static test_result_t tests[] = {
		{"{\"elemen\":null}", NGX_JSON_NOT_FOUND},
		{"{\"element\":[]}", NGX_JSON_BAD_TYPE},
		{"{\"element\":\"2xF1CWaBQs21ihR4NI-AwQ==\"}", NGX_JSON_BAD_DATA},
		{NULL, 0},
	};
	ngx_json_value_t result;
	test_result_t* cur_test;
	ngx_str_t str;
	ngx_int_t rc;
	
	for (cur_test = tests; cur_test->test; cur_test++)
	{
		rc = ngx_json_parse(NULL, (u_char*)cur_test->test, &result);
		assert(rc == NGX_JSON_OK);
		
		rc = ngx_json_get_element_binary_string(NULL, &result, &element, &str);
		if (rc != cur_test->expected_rc)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->test, cur_test->expected_rc, rc);
		}
	}
}

void get_array_tests()
{
	static test_result_t tests[] = {
		{"{\"elemen\":null}", NGX_JSON_NOT_FOUND},
		{"{\"element\":{}}", NGX_JSON_BAD_TYPE},
		{NULL, 0},
	};
	ngx_json_value_t result;
	test_result_t* cur_test;
	ngx_array_t* arr;
	ngx_int_t rc;

	for (cur_test = tests; cur_test->test; cur_test++)
	{
		rc = ngx_json_parse(NULL, (u_char*)cur_test->test, &result);
		assert(rc == NGX_JSON_OK);
		
		rc = ngx_json_get_element_array(&result, &element, &arr);
		if (rc != cur_test->expected_rc)
		{
			printf("Error: %s - expected %" PRIdPTR " got %" PRIdPTR "\n", cur_test->test, cur_test->expected_rc, rc);
		}
	}
}

int main()
{
	sanity_tests();
	bad_jsons_test();
	get_element_guid_tests();
	get_fixed_string_tests();
	get_binary_string_tests();
	get_array_tests();
	return 0;
}
